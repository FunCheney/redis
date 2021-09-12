### Hash表
首先在 Redis 中 Hash 表既是一种值类型，同时，Redis 也使用一种全局 Hash 表来保存所有键值对，从而满足应用存取 Hash 结构的数据需求，
又能提供快速的查询功能。

** Redis 如何解决 哈希冲突和 rehash 开销的？**

  * 针对哈希冲突，redis 采用了链式哈希，在不扩容的前提下，将具有相同哈希值的数据链接起来，以便这些数据在表中仍然可以查到。
  * 对于 rehash 开销，redis 实现了渐进式 rehash 设计，进而缓解了 rehash 操作带来的额外开销对系统性能的影响。

#### 如何解决 Hash 冲突

##### 1. 链式 Hash
所谓的链式 Hash 就是用一个链表把映射到 Hash 表同一个桶中的建给连接起来。在 Redis 中 Hash 表实现的文件主要是 dict.h 和 dict.c.
其中 dict.h 定义了 Hash 表的结构、哈希项，以及 Hash 表的各种操作函数，而 dict.c 文件中包含了 Hash 表中各种操作的具体代码实现。

dict.h 中对 Hash 表的定义
```c++
typedef struct dictht {
    // 二维数组
    dictEntry **table;
    // Hash 表大小
    unsigned long size;
    unsigned long sizemask;
    unsigned long used;
} dictht
```
hash 表被定义为一个二维数组（dictEntry **table），这个数组中的每个元素是一个指向哈希项（dictEntry）的指指针。为了实现链式哈希，Redis 在每个
dictEntry 的结构设计中，出了包含指向键和值的指针，还包含了指向下一个哈希项的指针（dictEntry *next）。
```c++
typedef struct dictEntry {
    // 指向键 的指针
    void *key;
    // 定义联合体 v
    union {
        // 指向实际值的指针
        void *val;
        // 无符号 64 位整数
        uint64_t u64;
        // 有符号64 位整数
        int64_t s64;
        // double 类的值
        double d;
    } v;
    // 指向另一个 dictEntry 结构体的指针
    struct dictEntry *next;
} dictEntry;
```
可以看到在 dictEntry 结构体中，键值对的值是由一个联合体 v 定义的。其中包含了指向实际值的指针（*val），还包含了无符号的 64 位整数，有符号的 64 位整数，
以及 double 类型的值。**这种实现方法是一种节省内存的开发小技巧。因为当值为整数或双精度浮点数时，由于其本身就是 64 位，就可以不用指针指向了，
而是可以直接存在键值对的结构体中，这样就避免了再用一个指针，从而节省了内存空间。**

**局限性**

随着链表长度的增加，Hash 表在同一个位置上查询哈希表项的耗时就会增加，从而增加整个查询时间，这样会导致 Hash 表的性能下降。

##### 2. reHash

* 首先，准备两个哈希表，用于 rehash 时交替保存数据。Redis 在实际使用 Hash 表时，Redis 定义了一个 dict 结构体。这个结构体中有一个数组（ht[2]）,
包含了两个 Hash 表 ht[0] 和 ht[1]。
```c++
typedef struct dict {
    dictType *type;
    void *privdata;
    // 两个 hash 表，交替使用，用于 rehash 操作
    dictht ht[2];
    // Hash 表是否在进行 rehash 的标识， -1表示没有进行 rehash
    long rehashidx; /* rehashing not in progress if rehashidx == -1 */
    unsigned long iterators; /* number of iterators currently running */
} dict;
```
* 其次，在正常服务请求阶段，所有的键值对写入哈希表 ht[0]。
* 接着，当进行 rehash 时，键值对被迁移到哈希表 ht[1] 中。
* 最后，当迁移完成，ht[0] 的恐慌家会被释放，并把 ht[1] 的地址赋值给 ht[0], ht[1] 的表大小设置为0。这样一来，又回到了正常服务请求的阶段，ht[0] 接受服务请求，
ht[1] 作为下一次 rehash 时的迁移表。
  
**什么时候触发 rehash**

在 Redis 中用来判断是否触发 rehash 的函数是 **_dictExpandIfNeeded**，下面就来看看 _dictExpandIfNeeded 函数中进行扩容的触发条件。
```c++
static int _dictExpandIfNeeded(dict *d)
{
    /* Incremental rehashing already in progress. Return. */
    if (dictIsRehashing(d)) return DICT_OK;
    
    // 如果 hash 表为空，将 Hash 表扩为默认大小
    if (d->ht[0].size == 0) return dictExpand(d, DICT_HT_INITIAL_SIZE);

     // 如果当前 hash 表承载的元素个数超过其当前大小，并且可以进行扩容或者 Hash 表承载的元素个数已是当前大小的5倍
    if (d->ht[0].used >= d->ht[0].size &&
        (dict_can_resize ||
         d->ht[0].used/d->ht[0].size > dict_force_resize_ratio))
    {
        return dictExpand(d, d->ht[0].used*2);
    }
    return DICT_OK;
}
```
* 条件一：ht[0] 的大小为 0。
* 条件二：ht[0] 承载的元素个数已经超过了 ht[0] 的大小， 同时 Hash 表可以进行扩容。
* 条件三：ht[0] 承载的元素个数是 ht[0] 的大小的 dict_force_resize_ratio 倍，其中 dict_force_resize_ratio 的默认值是 5。


**允许/禁止 扩容？**

在 dict.c 中有如下两个函数用老设置是否允许扩容
```c++
void dictEnableResize(void) {
    // 允许扩容
    dict_can_resize = 1;
}

void dictDisableResize(void) {
    // 不允许扩容
    dict_can_resize = 0;
}
```
然后，这两个函数又被封装在了 updateDictResizePolicy 函数中(在server.c 文件中)。
```c++
void updateDictResizePolicy(void) {
    if (server.rdb_child_pid == -1 && server.aof_child_pid == -1)
        dictEnableResize();
    else
        dictDisableResize();
}
```
在这个函数中调用 **dictEnableResize** 函数功能的条件是：**当前有没有 RDB 子进程，并且没有 AOF 子进程。** 这就对应了 Redis 没有执行 RDB 快照
和没有进行 AOF 重写的场景。

**rehash 场景**

_dictExpandIfNeeded 的被调用关系，我们可以发现，_dictExpandIfNeeded 是被 _dictKeyIndex 函数调用的，而 _dictKeyIndex 函数又会被 dictAddRaw 函数调用，然后 dictAddRaw 会被以下三个函数调用。

* dictAdd：用来往 Hash 表中添加一个键值对。
* dictRelace：用来往 Hash 表中添加一个键值对，或者键值对存在时，修改键值对。
* dictAddorFind：直接调用 dictAddRaw。

**总结：**

Redis 中触发 rehash 操作的关键，就是 _dictExpandIfNeeded 函数和 updateDictResizePolicy 函数。_dictExpandIfNeeded 函数会根据 Hash 
表的负载因子以及能否进行 rehash 的标识，判断是否进行 rehash，而 updateDictResizePolicy 函数会根据 RDB 和 AOF 的执行情况，启用或禁用 rehash。



##### 3.渐进式 rehash 如何实现？

**为什么要实现渐进式 rehash？**

Hash 表在执行 rehash 时，由于 Hash 表空间扩大，原本映射到某一位置的键可能会被映射到一个新的位置上，因此，很多键就需要从原来的位置拷贝到新的位置。
而在键拷贝时，由于 Redis 主线程无法执行其他的请求，所以拷贝键会阻塞主线程，这样就会产生 **rehash 开销。**

为了降低 rehash 的开销，Redis 就提出了渐进式 rehash 的方法。

渐进式 rehash 的意思就是 Redis 并不会一次性把当前 Hash 表中的所有键，都拷贝到新位置，而是会分批拷贝，每次的键拷贝只拷贝 Hash 表中一个 bucket 中的哈希项。这样一来，每次键拷贝的时长有限，对主线程的影响也就有限了。


**渐进式 rehash 在代码层面是如何实现?**

这里有两个关键函数：dictRehash 和 _dictRehashStep。

dictRehash 函数实际执行键拷贝，它的输入参数有两个，分别是全局 hash 表（即前面提到的 dict 结构体，包含了 ht[0]和 ht[1]）和需要进行键拷贝的桶数量（bucket 数量）。

```c++
int dictRehash(dict *d, int n) {
    int empty_visits = n*10; /* Max number of empty buckets to visit. */
    if (!dictIsRehashing(d)) return 0;

    // 主循环，根据要拷贝的 bucket数量n，循环n次后停止或ht[0]中的数据迁移完停止
    while(n-- && d->ht[0].used != 0) {
        dictEntry *de, *nextde;

        /* Note that rehashidx can't overflow as we are sure there are more
         * elements because ht[0].used != 0 */
         // rehashidx 变量表示当前 reHash 是在对那个 bucket 做数据迁移，等于 0 时，表示对 ht[0]中的第一个 bucket 进行数据迁移；
         //                                      当 rehashidx 等于 1 时，表示对 ht[0]中的第二个 bucket 进行数据迁移，以此类推。
        assert(d->ht[0].size > (unsigned long)d->rehashidx);
        // 判断 rehashidx 指向的 bucket 是否为空
        while(d->ht[0].table[d->rehashidx] == NULL) {
            // 为空， rehashidx 的值加 1，检查下一个 bucket
            d->rehashidx++;
            // 渐进式 rehash 在执行时设置了一个变量 empty_visits，用来表示已经检查过的空 bucket，
            // 当检查了一定数量的空 bucket 后，这一轮的 rehash 就停止执行，转而继续处理外来请求，避免了对 Redis 性能的影响
            if (--empty_visits == 0) return 1;
        }
        // rehashidx 指向的 bucket 有数据可以迁移
        // 获取 Hash 表中的 哈希项
        de = d->ht[0].table[d->rehashidx];
        /* Move all the keys in this bucket from the old to the new hash HT */
        while(de) {
            uint64_t h;
            // 获取同一个 bucket 中的下一个哈希项
            nextde = de->next;
            /* Get the index in the new hash table */
            // 根据扩容后的哈希表 ht[1] 大小，计算桑倩哈希项在扩容后哈希表中的 bucket 位置
            h = dictHashKey(d, de->key) & d->ht[1].sizemask;
            // 将当前的 hash 项添加到扩容后的哈希表 ht[1] 中
            de->next = d->ht[1].table[h];
            d->ht[1].table[h] = de;
            // 减少当前哈希表中 哈希项的个数
            d->ht[0].used--;
            // 增加扩容后哈希表的哈希项的个数
            d->ht[1].used++;
            // 指向洗下一个哈希项
            de = nextde;
        }
        //如果当前bucket中已经没有哈希项了，将该bucket置为NULL，释放内存空间
        d->ht[0].table[d->rehashidx] = NULL;
        // 将rehash加1，下一次将迁移下一个bucket中的元素
        d->rehashidx++;
    }

    /* Check if we already rehashed the whole table... */
    // 判断 ht[0] 的数据是否迁移完成
    if (d->ht[0].used == 0) {
        // ht[0] 迁移完成后释放 ht[0] 内存空间
        zfree(d->ht[0].table);
        // 让 ht[0] 指向 ht[1], 以便正常接收请求
        d->ht[0] = d->ht[1];
        // 重置 ht[1] 的大小为 0
        _dictReset(&d->ht[1]);
        // 设置全局哈希表的 rehashidx标识为 -1，表示 rehash 结束
        d->rehashidx = -1;
        // 返回0， 表示 ht[0] 中所有元素都迁移完
        return 0;
    }

    /* More to rehash... */
    // 返回 1 表示 ht[1] 中仍然有元素没有迁移完
    return 1;
}
```
总结：

dictRehash 函数的整体逻辑包括两部分：

* 首先，该函数会执行一个循环，根据要进行键拷贝的 bucket 数量 n，依次完成这些 bucket 内部所有键的迁移。当然，如果 ht[0]哈希表中的数据已经都迁移完成了，键拷贝的循环也会停止执行。

* 其次，在完成了 n 个 bucket 拷贝后，dictRehash 函数的第二部分逻辑，就是判断 ht[0]表中数据是否都已迁移完。如果都迁移完了，那么 ht[0]的空间会被释放。因为 Redis 在处理请求时，代码逻辑中都是使用 
ht[0]，所以当 rehash 执行完成后，虽然数据都在 ht[1]中了，但 Redis 仍然会把 ht[1]赋值给 ht[0]，以便其他部分的代码逻辑正常使用。
  
* 而在 ht[1]赋值给 ht[0]后，它的大小就会被重置为 0，等待下一次 rehash。与此同时，全局哈希表中的 rehashidx 变量会被标为 -1，表示 rehash 结束了。


**渐进式 rehash 相关的第二个关键函数 _dictRehashStep，这个函数实现了每次只对一个 bucket 执行 rehash。**

从 Redis 的源码中可以看到，一共会有 5 个函数通过调用 _dictRehashStep 函数，进而调用 dictRehash 函数，来执行 rehash，它们分别是：dictAddRaw，
dictGenericDelete，dictFind，dictGetRandomKey，dictGetSomeKeys。 其中，dictAddRaw 和 dictGenericDelete 函数，分别对应了往 Redis 中增加和删除键值对，而后三个函数则对应了在 Redis 中进行查询操作

不管是增删查哪种操作，这 5 个函数调用的 _dictRehashStep 函数，给 dictRehash 传入的拷贝的 bucket 的数量都是 1，下面的代码就显示了这一传参的情况。
```c++
static void _dictRehashStep(dict *d) {
    // 给 dictRehash 传入的循环次数为1，表明每迁移完一个 bucket，就执行正常操作。 
    if (d->iterators == 0) dictRehash(d,1);
}
```


