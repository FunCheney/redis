/* Maxmemory directive handling (LRU eviction and other policies).
 *
 * ----------------------------------------------------------------------------
 *
 * Copyright (c) 2009-2016, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "server.h"
#include "bio.h"
#include "atomicvar.h"

/* ----------------------------------------------------------------------------
 * Data structures
 * --------------------------------------------------------------------------*/

/* To improve the quality of the LRU approximation we take a set of keys
 * that are good candidate for eviction across freeMemoryIfNeeded() calls.
 *
 * Entries inside the eviciton pool are taken ordered by idle time, putting
 * greater idle times to the right (ascending order).
 *
 * When an LFU policy is used instead, a reverse frequency indication is used
 * instead of the idle time, so that we still evict by larger value (larger
 * inverse frequency means to evict keys with the least frequent accesses).
 *
 * Empty entries have the key pointer set to NULL. */
#define EVPOOL_SIZE 16
#define EVPOOL_CACHED_SDS_SIZE 255
// 用来保存淘汰的候选键值对
struct evictionPoolEntry {
    // 记录待淘汰键值对的空闲时间
    unsigned long long idle;    /* Object idle time (inverse frequency for LFU) */
    // 待淘汰的键值对的key
    sds key;                    /* Key name. */
    // 缓存的SDS对象
    sds cached;                 /* Cached SDS object for key name. */
    // 待淘汰键值对的key所在的数据库ID
    int dbid;                   /* Key DB number. */
};

static struct evictionPoolEntry *EvictionPoolLRU;

/* ----------------------------------------------------------------------------
 * Implementation of eviction, aging and LRU
 * --------------------------------------------------------------------------*/

/* Return the LRU clock, based on the clock resolution. This is a time
 * in a reduced-bits format that can be used to set and check the
 * object->lru field of redisObject structures. */
unsigned int getLRUClock(void) {
    // mstime() 获得以 毫秒为单位的 unix 时间戳
    // LRU_CLOCK_RESOLUTION 表示以毫秒为单位的时间 LRU 时钟精度，也就是以毫秒为单位来表示 LRU 时钟的最小单位
    // LRU_CLOCK_MAX 表示 lRU 的最大时钟值
    return (mstime()/LRU_CLOCK_RESOLUTION) & LRU_CLOCK_MAX;
}

/* This function is used to obtain the current LRU clock.
 * If the current resolution is lower than the frequency we refresh the
 * LRU clock (as it should be in production servers) we return the
 * precomputed value, otherwise we need to resort to a system call. */
unsigned int LRU_CLOCK(void) {
    unsigned int lruclock;
    if (1000/server.hz <= LRU_CLOCK_RESOLUTION) {
        atomicGet(server.lruclock,lruclock);
    } else {
        lruclock = getLRUClock();
    }
    return lruclock;
}

/* Given an object returns the min number of milliseconds the object was never
 * requested, using an approximated LRU algorithm. */
unsigned long long estimateObjectIdleTime(robj *o) {
    unsigned long long lruclock = LRU_CLOCK();
    if (lruclock >= o->lru) {
        return (lruclock - o->lru) * LRU_CLOCK_RESOLUTION;
    } else {
        return (lruclock + (LRU_CLOCK_MAX - o->lru)) *
                    LRU_CLOCK_RESOLUTION;
    }
}

/* freeMemoryIfNeeded() gets called when 'maxmemory' is set on the config
 * file to limit the max memory used by the server, before processing a
 * command.
 *
 * The goal of the function is to free enough memory to keep Redis under the
 * configured memory limit.
 *
 * The function starts calculating how many bytes should be freed to keep
 * Redis under the limit, and enters a loop selecting the best keys to
 * evict accordingly to the configured policy.
 *
 * If all the bytes needed to return back under the limit were freed the
 * function returns C_OK, otherwise C_ERR is returned, and the caller
 * should block the execution of commands that will result in more memory
 * used by the server.
 *
 * ------------------------------------------------------------------------
 *
 * LRU approximation algorithm
 *
 * Redis uses an approximation of the LRU algorithm that runs in constant
 * memory. Every time there is a key to expire, we sample N keys (with
 * N very small, usually in around 5) to populate a pool of best keys to
 * evict of M keys (the pool size is defined by EVPOOL_SIZE).
 *
 * The N keys sampled are added in the pool of good keys to expire (the one
 * with an old access time) if they are better than one of the current keys
 * in the pool.
 *
 * After the pool is populated, the best key we have in the pool is expired.
 * However note that we don't remove keys from the pool when they are deleted
 * so the pool may contain keys that no longer exist.
 *
 * When we try to evict a key, and all the entries in the pool don't exist
 * we populate it again. This time we'll be sure that the pool has at least
 * one key that can be evicted, if there is at least one key that can be
 * evicted in the whole database. */

/* Create a new eviction pool. */
void evictionPoolAlloc(void) {
    struct evictionPoolEntry *ep;
    int j;

    ep = zmalloc(sizeof(*ep)*EVPOOL_SIZE);
    for (j = 0; j < EVPOOL_SIZE; j++) {
        ep[j].idle = 0;
        ep[j].key = NULL;
        ep[j].cached = sdsnewlen(NULL,EVPOOL_CACHED_SDS_SIZE);
        ep[j].dbid = 0;
    }
    EvictionPoolLRU = ep;
}

/* This is an helper function for freeMemoryIfNeeded(), it is used in order
 * to populate the evictionPool with a few entries every time we want to
 * expire a key. Keys with idle time smaller than one of the current
 * keys are added. Keys are always added if there are free entries.
 *
 * We insert keys on place in ascending order, so keys with the smaller
 * idle time are on the left, and keys with the higher idle time on the
 * right. */

void evictionPoolPopulate(int dbid, dict *sampledict, dict *keydict, struct evictionPoolEntry *pool) {
    int j, k, count;
    // 采样后的集合，大小为 maxmemory_samples
    dictEntry *samples[server.maxmemory_samples];
    // 将待采样的哈希表 sampledict、采样后的集合 samples、以及采样数量 maxmemory_samples，作为参数传给 dictGetSomeKeys
    count = dictGetSomeKeys(sampledict,samples,server.maxmemory_samples);
    // dictGetSomeKeys 函数就能返回采样的键值对集合了, 根据实际采样到的键值对数量 count，执行一个循环。
    for (j = 0; j < count; j++) {
        unsigned long long idle;
        sds key;
        robj *o;
        dictEntry *de;

        de = samples[j];
        key = dictGetKey(de);

        /* If the dictionary we are sampling from is not the main
         * dictionary (but the expires one) we need to lookup the key
         * again in the key dictionary to obtain the value object. */
        if (server.maxmemory_policy != MAXMEMORY_VOLATILE_TTL) {
            if (sampledict != keydict) de = dictFind(keydict, key);
            o = dictGetVal(de);
        }

        /* Calculate the idle time according to the policy. This is called
         * idle just because the code initially handled LRU, but is in fact
         * just a score where an higher score means better candidate. */
        if (server.maxmemory_policy & MAXMEMORY_FLAG_LRU) {
            // 计算在采样集合中的每一个键值对的空闲时间
            idle = estimateObjectIdleTime(o);
        } else if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
            /* When we use an LRU policy, we sort the keys by idle time
             * so that we expire keys starting from greater idle time.
             * However when the policy is an LFU one, we have a frequency
             * estimation, and we want to evict keys with lower frequency
             * first. So inside the pool we put objects using the inverted
             * frequency subtracting the actual frequency to the maximum
             * frequency of 255. */
            // 用 255 减去键值对的访问次数，这样来计算 EvictionPoolLRU 数组中每个元素的 idle 变量值
            idle = 255-LFUDecrAndReturn(o);
        } else if (server.maxmemory_policy == MAXMEMORY_VOLATILE_TTL) {
            /* In this case the sooner the expire the better. */
            idle = ULLONG_MAX - (long)dictGetVal(de);
        } else {
            serverPanic("Unknown eviction policy in evictionPoolPopulate()");
        }

        /* Insert the element inside the pool.
         * First, find the first empty bucket or the first populated
         * bucket that has an idle time smaller than our idle time. */
        k = 0;
        while (k < EVPOOL_SIZE &&
               pool[k].key &&
               pool[k].idle < idle) k++;
        if (k == 0 && pool[EVPOOL_SIZE-1].key != NULL) {
            /* Can't insert if the element is < the worst element we have
             * and there are no empty buckets. */
            continue;
        } else if (k < EVPOOL_SIZE && pool[k].key == NULL) {
            /* Inserting into empty position. No setup needed before insert. */
        } else {
            /* Inserting in the middle. Now k points to the first element
             * greater than the element to insert.  */
            if (pool[EVPOOL_SIZE-1].key == NULL) {
                /* Free space on the right? Insert at k shifting
                 * all the elements from k to end to the right. */

                /* Save SDS before overwriting. */
                sds cached = pool[EVPOOL_SIZE-1].cached;
                memmove(pool+k+1,pool+k,
                    sizeof(pool[0])*(EVPOOL_SIZE-k-1));
                pool[k].cached = cached;
            } else {
                /* No free space on right? Insert at k-1 */
                k--;
                /* Shift all elements on the left of k (included) to the
                 * left, so we discard the element with smaller idle time. */
                sds cached = pool[0].cached; /* Save SDS before overwriting. */
                if (pool[0].key != pool[0].cached) sdsfree(pool[0].key);
                memmove(pool,pool+1,sizeof(pool[0])*k);
                pool[k].cached = cached;
            }
        }

        /* Try to reuse the cached SDS string allocated in the pool entry,
         * because allocating and deallocating this object is costly
         * (according to the profiler, not my fantasy. Remember:
         * premature optimizbla bla bla bla. */
        int klen = sdslen(key);
        if (klen > EVPOOL_CACHED_SDS_SIZE) {
            pool[k].key = sdsdup(key);
        } else {
            memcpy(pool[k].cached,key,klen+1);
            sdssetlen(pool[k].cached,klen);
            pool[k].key = pool[k].cached;
        }
        pool[k].idle = idle;
        pool[k].dbid = dbid;
    }
}

/* ----------------------------------------------------------------------------
 * LFU (Least Frequently Used) implementation.

 * We have 24 total bits of space in each object in order to implement
 * an LFU (Least Frequently Used) eviction policy, since we re-use the
 * LRU field for this purpose.
 *
 * We split the 24 bits into two fields:
 *
 *          16 bits      8 bits
 *     +----------------+--------+
 *     + Last decr time | LOG_C  |
 *     +----------------+--------+
 *
 * LOG_C is a logarithmic counter that provides an indication of the access
 * frequency. However this field must also be decremented otherwise what used
 * to be a frequently accessed key in the past, will remain ranked like that
 * forever, while we want the algorithm to adapt to access pattern changes.
 *
 * So the remaining 16 bits are used in order to store the "decrement time",
 * a reduced-precision Unix time (we take 16 bits of the time converted
 * in minutes since we don't care about wrapping around) where the LOG_C
 * counter is halved if it has an high value, or just decremented if it
 * has a low value.
 *
 * New keys don't start at zero, in order to have the ability to collect
 * some accesses before being trashed away, so they start at COUNTER_INIT_VAL.
 * The logarithmic increment performed on LOG_C takes care of COUNTER_INIT_VAL
 * when incrementing the key, so that keys starting at COUNTER_INIT_VAL
 * (or having a smaller value) have a very high chance of being incremented
 * on access.
 *
 * During decrement, the value of the logarithmic counter is halved if
 * its current value is greater than two times the COUNTER_INIT_VAL, otherwise
 * it is just decremented by one.
 * --------------------------------------------------------------------------*/

/* Return the current time in minutes, just taking the least significant
 * 16 bits. The returned time is suitable to be stored as LDT (last decrement
 * time) for the LFU implementation. */
unsigned long LFUGetTimeInMinutes(void) {
    return (server.unixtime/60) & 65535;
}

/* Given an object last access time, compute the minimum number of minutes
 * that elapsed since the last access. Handle overflow (ldt greater than
 * the current 16 bits minutes time) considering the time as wrapping
 * exactly once. */
unsigned long LFUTimeElapsed(unsigned long ldt) {
    unsigned long now = LFUGetTimeInMinutes();
    if (now >= ldt) return now-ldt;
    return 65535-ldt+now;
}

/* Logarithmically increment a counter. The greater is the current counter value
 * the less likely is that it gets really implemented. Saturate it at 255. */
uint8_t LFULogIncr(uint8_t counter) {
    // 当前访问次数大于 255 返回 255 不在增加访问次数
    if (counter == 255) return 255;
    // 计算一个 0 到 1 之间的随机值
    double r = (double)rand()/RAND_MAX;
    // baseval 的大小：这反映了当前访问次数的多少。比如，访问次数越多的键值对，它的访问次数再增加的难度就会越大；
    double baseval = counter - LFU_INIT_VAL;
    if (baseval < 0) baseval = 0;
    // 计算阈值
    double p = 1.0/(baseval*server.lfu_log_factor+1);
    // 如果概率值 r 小于阈值 p，访问次数 +1
    if (r < p) counter++;
    return counter;
}

/* If the object decrement time is reached decrement the LFU counter but
 * do not update LFU fields of the object, we update the access time
 * and counter in an explicit way when the object is really accessed.
 * And we will times halve the counter according to the times of
 * elapsed time than server.lfu_decay_time.
 * Return the object frequency counter.
 *
 * This function is used in order to scan the dataset for the best object
 * to fit: as we check for the candidate, we incrementally decrement the
 * counter of the scanned objects if needed. */
unsigned long LFUDecrAndReturn(robj *o) {
    unsigned long ldt = o->lru >> 8; // 获取当前键值对的上一次访问时间
    unsigned long counter = o->lru & 255;// 获取当前的访问次数
    unsigned long num_periods = server.lfu_decay_time ? LFUTimeElapsed(ldt) / server.lfu_decay_time : 0;
    if (num_periods) // 如果衰减大小不为 0
        // 如果衰减大小 小于当前的访问次数，那么，衰减后的访问次数是当前访问次数的大小减去衰减大小，否则，衰减后的访问次数为 0
        counter = (num_periods > counter) ? 0 : counter - num_periods;
    return counter;
}

/* ----------------------------------------------------------------------------
 * The external API for eviction: freeMemroyIfNeeded() is called by the
 * server when there is data to add in order to make space if needed.
 * --------------------------------------------------------------------------*/

/* We don't want to count AOF buffers and slaves output buffers as
 * used memory: the eviction should use mostly data size. This function
 * returns the sum of AOF and slaves buffer. */
size_t freeMemoryGetNotCountedMemory(void) {
    size_t overhead = 0;
    int slaves = listLength(server.slaves);

    if (slaves) {
        listIter li;
        listNode *ln;

        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            client *slave = listNodeValue(ln);
            overhead += getClientOutputBufferMemoryUsage(slave);
        }
    }
    if (server.aof_state != AOF_OFF) {
        overhead += sdsalloc(server.aof_buf)+aofRewriteBufferSize();
    }
    return overhead;
}

/* Get the memory status from the point of view of the maxmemory directive:
 * if the memory used is under the maxmemory setting then C_OK is returned.
 * Otherwise, if we are over the memory limit, the function returns
 * C_ERR.
 *
 * The function may return additional info via reference, only if the
 * pointers to the respective arguments is not NULL. Certain fields are
 * populated only when C_ERR is returned:
 *
 *  'total'     total amount of bytes used.
 *              (Populated both for C_ERR and C_OK)
 *
 *  'logical'   the amount of memory used minus the slaves/AOF buffers.
 *              (Populated when C_ERR is returned)
 *
 *  'tofree'    the amount of memory that should be released
 *              in order to return back into the memory limits.
 *              (Populated when C_ERR is returned)
 *
 *  'level'     this usually ranges from 0 to 1, and reports the amount of
 *              memory currently used. May be > 1 if we are over the memory
 *              limit.
 *              (Populated both for C_ERR and C_OK)
 */
int getMaxmemoryState(size_t *total, size_t *logical, size_t *tofree, float *level) {
    size_t mem_reported, mem_used, mem_tofree;

    /* Check if we are over the memory usage limit. If we are not, no need
     * to subtract the slaves output buffers. We can just return ASAP. */
    mem_reported = zmalloc_used_memory(); // 计算已经使用的内存空间
    if (total) *total = mem_reported;

    /* We may return ASAP if there is no need to compute the level. */
    int return_ok_asap = !server.maxmemory || mem_reported <= server.maxmemory;
    if (return_ok_asap && !level) return C_OK;

    /* Remove the size of slaves output buffers and AOF buffer from the
     * count of used memory. */
    // 将用于主从复制的复制缓冲区大小从已使用内存中扣除
    mem_used = mem_reported;
    size_t overhead = freeMemoryGetNotCountedMemory();
    mem_used = (mem_used > overhead) ? mem_used-overhead : 0;

    /* Compute the ratio of memory usage. */
    if (level) {
        if (!server.maxmemory) {
            *level = 0;
        } else {
            *level = (float)mem_used / (float)server.maxmemory;
        }
    }

    if (return_ok_asap) return C_OK;

    /* Check if we are still over the memory limit. */
    if (mem_used <= server.maxmemory) return C_OK;

    /* Compute how much memory we need to free. */
    // 计算需要释放的存
    mem_tofree = mem_used - server.maxmemory;

    if (logical) *logical = mem_used;
    if (tofree) *tofree = mem_tofree;

    return C_ERR;
}

/* This function is periodically called to see if there is memory to free
 * according to the current "maxmemory" settings. In case we are over the
 * memory limit, the function will try to free some memory to return back
 * under the limit.
 *
 * The function returns C_OK if we are under the memory limit or if we
 * were over the limit, but the attempt to free memory was successful.
 * Otehrwise if we are over the memory limit, but not enough memory
 * was freed to return back under the limit, the function returns C_ERR. */
int freeMemoryIfNeeded(void) {
    /* By default replicas should ignore maxmemory
     * and just be masters exact copies. */
    if (server.masterhost && server.repl_slave_ignore_maxmemory) return C_OK;

    size_t mem_reported, mem_tofree, mem_freed;
    mstime_t latency, eviction_latency;
    long long delta;
    int slaves = listLength(server.slaves);

    /* When clients are paused the dataset should be static not just from the
     * POV of clients not being able to write, but also from the POV of
     * expires and evictions of keys not being performed. */
    if (clientsArePaused()) return C_OK;
    // getMaxmemoryState 评估当前内存的使用情况
    if (getMaxmemoryState(&mem_reported,NULL,&mem_tofree,NULL) == C_OK)
        // 如果当前内存使用量没有超过 maxmemory，那么，getMaxmemoryState 函数会返回 C_OK
        // 紧接着，freeMemoryIfNeeded 函数也会直接返回了。
        return C_OK;

    mem_freed = 0;

    if (server.maxmemory_policy == MAXMEMORY_NO_EVICTION)
        goto cant_free; /* We need to free memory, but policy forbids. */

    latencyStartMonitor(latency);
    // 而如果当前 server 使用的内存量，的确已经超出 maxmemory 的上限了，通过 while 循环，来淘汰数据释放内存
    while (mem_freed < mem_tofree) {
        int j, k, i, keys_freed = 0;
        static unsigned int next_db = 0;
        sds bestkey = NULL;
        int bestdbid;
        redisDb *db;
        dict *dict;
        dictEntry *de;

        if (server.maxmemory_policy & (MAXMEMORY_FLAG_LRU|MAXMEMORY_FLAG_LFU) ||
            server.maxmemory_policy == MAXMEMORY_VOLATILE_TTL)
        {
            struct evictionPoolEntry *pool = EvictionPoolLRU;

            while(bestkey == NULL) {
                unsigned long total_keys = 0, keys;

                /* We don't want to make local-db choices when expiring keys,
                 * so to start populate the eviction pool sampling keys from
                 * every DB. */
                for (i = 0; i < server.dbnum; i++) {
                    db = server.db+i; // Redis server上的每一个数据库都执行
                    // 根据淘汰策略，决定使用全局的哈希表还是设置了过期时间的哈希表
                    dict = (server.maxmemory_policy & MAXMEMORY_FLAG_ALLKEYS) ?
                            db->dict : db->expires;
                    if ((keys = dictSize(dict)) != 0) {
                        // 更新淘汰的候选键值对集合
                        // 将选择的哈希表dict传入evictionPoolPopulate函数，同时将全局哈希表也传给evictionPoolPopulate函数
                        evictionPoolPopulate(i, dict, db->dict, pool);
                        total_keys += keys;
                    }
                }
                if (!total_keys) break; /* No keys to evict. */

                /* Go backward from best to worst element to evict. */
                // 选择淘汰的键值对并删除
                // evictionPoolPopulate 函数已经更新了 EvictionPoolLRU 数组，而且这个数组里面的 key，是按照空闲时间从小到大排好序了
                for (k = EVPOOL_SIZE-1; k >= 0; k--) { // 从最后一个元素开始查找
                    if (pool[k].key == NULL) continue; // 当前 key 值为空，则查找下一个 key
                    bestdbid = pool[k].dbid;

                    // 从全局哈希表或是expire哈希表中，获取当前key对应的键值对；
                    if (server.maxmemory_policy & MAXMEMORY_FLAG_ALLKEYS) {
                        de = dictFind(server.db[pool[k].dbid].dict,
                            pool[k].key);
                    } else {
                        de = dictFind(server.db[pool[k].dbid].expires,
                            pool[k].key);
                    }

                    /* Remove the entry from the pool. */
                    // 将当前key从EvictionPoolLRU数组删除
                    if (pool[k].key != pool[k].cached)
                        sdsfree(pool[k].key);
                    pool[k].key = NULL;
                    pool[k].idle = 0;

                    /* If the key exists, is our pick. Otherwise it is
                     * a ghost and we need to try the next element. */
                    // 如果当前key对应的键值对不为空，选择当前key为被淘汰的key
                    if (de) {
                        bestkey = dictGetKey(de);
                        break;
                    } else { // 否则，继续查找下个key
                        /* Ghost... Iterate again. */
                    }
                }
            }
        }

        /* volatile-random and allkeys-random policy */
        else if (server.maxmemory_policy == MAXMEMORY_ALLKEYS_RANDOM ||
                 server.maxmemory_policy == MAXMEMORY_VOLATILE_RANDOM)
        {
            /* When evicting a random key, we try to evict a key for
             * each DB, so we use the static 'next_db' variable to
             * incrementally visit all DBs. */
            for (i = 0; i < server.dbnum; i++) {
                j = (++next_db) % server.dbnum;
                db = server.db+j;
                dict = (server.maxmemory_policy == MAXMEMORY_ALLKEYS_RANDOM) ?
                        db->dict : db->expires;
                if (dictSize(dict) != 0) {
                    de = dictGetRandomKey(dict);
                    bestkey = dictGetKey(de);
                    bestdbid = j;
                    break;
                }
            }
        }

        /* Finally remove the selected key. */
        // 最后删除被选择的 key
        if (bestkey) {
            db = server.db+bestdbid;
            // 为被淘汰的对象创建一个结构体
            robj *keyobj = createStringObject(bestkey,sdslen(bestkey));
            propagateExpire(db,keyobj,server.lazyfree_lazy_eviction);
            /* We compute the amount of memory freed by db*Delete() alone.
             * It is possible that actually the memory needed to propagate
             * the DEL in AOF and replication link is greater than the one
             * we are freeing removing the key, but we can't account for
             * that otherwise we would never exit the loop.
             *
             * AOF and Output buffer memory will be freed eventually so
             * we only care about memory used by the key space. */
            // 再删除之前先调用 zmalloc_used_memory 计算此时内存的使用量
            delta = (long long) zmalloc_used_memory();
            latencyStartMonitor(eviction_latency);
            // 如果配置了惰性删除，则进行异步删除
            if (server.lazyfree_lazy_eviction)
                dbAsyncDelete(db,keyobj);
            else // 否则同步删除
                dbSyncDelete(db,keyobj);
            latencyEndMonitor(eviction_latency);
            latencyAddSampleIfNeeded("eviction-del",eviction_latency);
            latencyRemoveNestedEvent(latency,eviction_latency);
            // 删除完成后调用 zmalloc_used_memory 计算数据删除后的内存使用量
            delta -= (long long) zmalloc_used_memory();
            // 更新已释放的内存
            mem_freed += delta;
            server.stat_evictedkeys++;
            notifyKeyspaceEvent(NOTIFY_EVICTED, "evicted",
                keyobj, db->id);
            decrRefCount(keyobj);
            keys_freed++;

            /* When the memory to free starts to be big enough, we may
             * start spending so much time here that is impossible to
             * deliver data to the slaves fast enough, so we force the
             * transmission here inside the loop. */
            if (slaves) flushSlavesOutputBuffers();

            /* Normally our stop condition is the ability to release
             * a fixed, pre-computed amount of memory. However when we
             * are deleting objects in another thread, it's better to
             * check, from time to time, if we already reached our target
             * memory, since the "mem_freed" amount is computed only
             * across the dbAsyncDelete() call, while the thread can
             * release the memory all the time. */
            // 如果使用了惰性删除，并且没删除 16 个 key后，统计下当前内存的使用量
            if (server.lazyfree_lazy_eviction && !(keys_freed % 16)) {
                // 计算当前内存的使用量是否超过最大内存使用量
                if (getMaxmemoryState(NULL,NULL,NULL,NULL) == C_OK) {
                    /* Let's satisfy our stop condition. */
                    mem_freed = mem_tofree; // 如果满足最大容量要求，让已释放内存量等于待释放内存量，以便结束循环
                }
            }
        }

        if (!keys_freed) {
            latencyEndMonitor(latency);
            latencyAddSampleIfNeeded("eviction-cycle",latency);
            goto cant_free; /* nothing to free... */
        }
    }
    latencyEndMonitor(latency);
    latencyAddSampleIfNeeded("eviction-cycle",latency);
    return C_OK;

cant_free:
    /* We are here if we are not able to reclaim memory. There is only one
     * last thing we can try: check if the lazyfree thread has jobs in queue
     * and wait... */
    while(bioPendingJobsOfType(BIO_LAZY_FREE)) {
        if (((mem_reported - zmalloc_used_memory()) + mem_freed) >= mem_tofree)
            break;
        usleep(1000);
    }
    return C_ERR;
}

/* This is a wrapper for freeMemoryIfNeeded() that only really calls the
 * function if right now there are the conditions to do so safely:
 *
 * - There must be no script in timeout condition.
 * - Nor we are loading data right now.
 *
 */
int freeMemoryIfNeededAndSafe(void) {
    // Lua 脚本在超时运行 或 Redis server 正在加载数据
    if (server.lua_timedout || server.loading) return C_OK;
    // 只有在上述两个条件都不成立的情况下，freeMemoryIfNeeded 函数才会被调用
    return freeMemoryIfNeeded();
}
