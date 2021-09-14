### Sorted Set
&ensp;&ensp; *为什么 Sorted Set 既能支持高效的范围查询，同时还能以 O(1) 复杂度获取元素权重值？*

&ensp;&ensp;这其实和 Sorted Set 的底层设计实现相关。 Sorted Set 支持范围查询，这是因为它核心数据结构采用了跳表，而它又能以常数复杂度获取
元素权重，这是因为它同时采用了哈希表进行索引。

#### Sorted Set 数据结构设计带来问题
* 跳表或哈希表中，各自保存什么样的数据？
  
* 跳表和哈希表保存的数据是如何保持一致的？

** zset 结构体**

```c++
// zset 结构体
typedef struct zset {
    // hash 表
    dict *dict;
    // 跳表
    zskiplist *zsl;
} zset;
```


#### 跳表的设计与实现

&ensp;&ensp;跳表是一种多层的有序链表。

##### 跳表

*结点数据结构:*

```c++
typedef struct zskiplistNode {
    // Sorted Set 中的元素
    sds ele;
    // 元素的权重
    double score;
    // 后向指针，便于从跳表的尾结点进行倒叙查找，每个结点中还保存了
    // 一个后向指针，指向该结点的前一个结点
    struct zskiplistNode *backward;
    // 结点的 level 数组，保存没层上前项指针和跨度
    // 该数组中的每一个元素代表了跳表中的一层
    struct zskiplistLevel {
        // 指向下一结点的前向指针，这就使得结点可以在某一层和后续的结点连接起来
        struct zskiplistNode *forward;
        // 跨度 记录结点在某一层上的 *forward 指针和该结点之间，跨越了 level0 上的几个结点
        unsigned long span;
    } level[];
} zskiplistNode;
```
在 Sorted Set 中既要保存元素，也要保存元素的权重，所以对应跳表加点的结构定义中，就对应了 SDS 类型的变量 ele，以及 double 类型的变量 score。此外，
为了方便于从跳表的尾结点进行倒叙查找，每个跳表中还保存了一个后向指针（*backward），指向该结点的前一个结点。

然后，因为跳表是一个多层的有序链表，每一层也是由多个结点通过指针连接起来的。因此在跳表结点的结构体定义中，还包含了一个 zskiplistLevel 结构体类型的
level 数组。

level 数组中的每一个元素对应了一个 zskiplistLevel 结构体，也对应了跳表的一层。在 zskiplistLevel 结构体中定义了一个指向下一个结点的前向指针（*forward），
这就使得结点可以在某一层上和后续结点连接起来。

zskiplistLevel 结构体中还定义了跨度，这是用来记录结点在某一层上的 *forwad 指针和该指针指向的接结点之间，跨越 level0 上的几个结点。

最后，因为跳表中的结点都是按序排列的，所以，对于跳表中的某个结点，我们可以把从头结点到该结点的查询路径上，各个结点所在查询层次上的 *forward 指针跨度，做一个累加。
这个累加值就可以用来计算该结点在整个跳表中的顺序，另外这个结构特点还可以用来实现 Sorted Set 的 rank 操作，比如 ZRANK，ZREVRANK 等。


*跳表数据结构:*

```c++
typedef struct zskiplist {
    // 跳表的头结点 尾结点
    struct zskiplistNode *header, *tail;
    // 跳表的长度
    unsigned long length;
    // 跳表的最大层数
    int level;
} zskiplist
```

因为跳表的每个结点都是通过指针连接起来的，所以我们在使用跳表时，只需要从跳表结构体中获得头结点或者尾结点，就可以通过结点指针访问到跳表中的各个结点。

#### 跳表查询结点
在跳表中查询一个结点时，跳表会先从头结点的最高层开始，查找下一个结点。在跳表中结点同时保存了元素和权重，所以跳表在比较结点是，相应的有如下两个判断条件：

1. 当查找到的结点保存的元素权重，比要查找的权重小时，跳表就会继续访问该层上的下一个结点。

2. 当查找到的结点保存元素权重，等于要查找的权重时，跳表会再检查该结点保存的 SDS 类型数据，是否比要查找的 SDS 数据小。如果结点数据小于要查找的数据时，
跳表仍然会继续访问该层上的下一个结点。
   
当上述两个条件都不满足时，跳表就会用到当前查找到的结点的 level 数组了。跳表会使用当前结点 level 数组里面的下一层指针，然后沿着下一层指针继续查找，这就相当
于跳到了下一层接着查找。

#### 跳表结点层数的设计
有了 level 数组之后，一个跳表结点就可以在多层上被访问到了。而一个结点的 level 数组的层数也就决定了，改结点可以在第几层被访问到。所以，当要决定结点的层数
时，实际上是要决定 level 数组具体有几层。

一种设计方法是，让每一层上的结点数约是下一层上结点数的一半：

这种设计方法带来的好处是，当跳表从最高层开始进行查找时，由于每一层结点数都约是下一层结点数的一半，这种查找过程就类似于二分查找，查找复杂度可以降低到 O(logN)。

种设计方法也会带来负面影响，那就是为了维持相邻两层上结点数的比例为 2:1，一旦有新的结点插入或是有结点被删除，那么插入或删除处的结点，及其后续结点的层数都需要进行调整，而这样就带来了额外的开销。


跳表在创建结点时，采用的是另一种设计方法，即随机生成每个结点的层数。此时，相邻两层链表上的结点数并不需要维持在严格的 2:1 关系。这样一来，当新插入一个结点时，只需要修改前后结点的指针，
而其他结点的层数就不需要随之改变了，这就降低了插入操作的复杂度。

在 Redis 源码中，跳表结点层数是由 zslRandomLevel 函数决定
```c++
int zslRandomLevel(void) {
	// 初始化层数为 1
    int level = 1;
	// 随机数的值为 0.25, 这里可以看出增加一层的概率是不超过 25%
    while ((random()&0xFFFF) < (ZSKIPLIST_P * 0xFFFF))
		// 随机数的值小于 0.25， 那么层数就加 1
        level += 1;
    return (level<ZSKIPLIST_MAXLEVEL) ? level : ZSKIPLIST_MAXLEVEL;
}`
```

#### 哈希表和跳表的组合使用
Reids 中创建一个 zset 代码中会相继调用 dictCreate 函数创建 zset 中的哈希表，以及调用 zslCreate 函数创建跳表
```c++
// 创建一个 zset
robj *createZsetObject(void) {
    zset *zs = zmalloc(sizeof(*zs));
    robj *o;
	// 创建一个 hash 表
    zs->dict = dictCreate(&zsetDictType,NULL);
	// 创建一个 skipList
    zs->zsl = zslCreate();
    o = createObject(OBJ_ZSET,zs);
    o->encoding = OBJ_ENCODING_SKIPLIST;
    return o;
}
```
这样，在 Sorted Set 中同时有了这两个索引结构以后，接下来，我们要想组合使用它们，就需要保持这两个索引结构中的数据一致了。

当往 Sorted Set 中插入数据时，zsetAdd 函数就会被调用。所以，我们可以通过阅读 Sorted Set 的元素添加函数 zsetAdd 来学习是和保持数据一致的。

```c++
int zsetAdd(robj *zobj, double score, sds ele, int *flags, double *newscore) {
    /* Turn options into simple to check vars. */
    int incr = (*flags & ZADD_INCR) != 0;
    int nx = (*flags & ZADD_NX) != 0;
    int xx = (*flags & ZADD_XX) != 0;
    *flags = 0; /* We'll return our response flags. */
    double curscore;

    /* NaN as input is an error regardless of all the other parameters. */
    if (isnan(score)) {
        *flags = ZADD_NAN;
        return 0;
    }

    /* Update the sorted set according to its encoding. */
    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *eptr;

        if ((eptr = zzlFind(zobj->ptr,ele,&curscore)) != NULL) {
            /* NX? Return, same element already exists. */
            if (nx) {
                *flags |= ZADD_NOP;
                return 1;
            }

            /* Prepare the score for the increment if needed. */
            if (incr) {
                score += curscore;
                if (isnan(score)) {
                    *flags |= ZADD_NAN;
                    return 0;
                }
                if (newscore) *newscore = score;
            }

            /* Remove and re-insert when score changed. */
            if (score != curscore) {
                zobj->ptr = zzlDelete(zobj->ptr,eptr);
                zobj->ptr = zzlInsert(zobj->ptr,ele,score);
                *flags |= ZADD_UPDATED;
            }
            return 1;
        } else if (!xx) {
            /* Optimize: check if the element is too large or the list
             * becomes too long *before* executing zzlInsert. */
            zobj->ptr = zzlInsert(zobj->ptr,ele,score);
            if (zzlLength(zobj->ptr) > server.zset_max_ziplist_entries ||
                sdslen(ele) > server.zset_max_ziplist_value)
                zsetConvert(zobj,OBJ_ENCODING_SKIPLIST);
            if (newscore) *newscore = score;
            *flags |= ZADD_ADDED;
            return 1;
        } else {
            *flags |= ZADD_NOP;
            return 1;
        }
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
		// zset 采用的是 skipList 的编码方式
        zset *zs = zobj->ptr;
        zskiplistNode *znode;
        dictEntry *de;
		// 在 hash 表中查找要插入的元素是否存在
        de = dictFind(zs->dict,ele);
        if (de != NULL) {
            /* NX? Return, same element already exists. */
            if (nx) {
                *flags |= ZADD_NOP;
                return 1;
            }
			// 从哈希表中查询元素的权重
            curscore = *(double*)dictGetVal(de);

            /* Prepare the score for the increment if needed. */
			// 如果需要更新元素的权重
            if (incr) {
				// 更新权重值
                score += curscore;
                if (isnan(score)) {
                    *flags |= ZADD_NAN;
                    return 0;
                }
                if (newscore) *newscore = score;
            }

            /* Remove and re-insert when score changes. */
			// 如果权重发生了变化
            if (score != curscore) {
				// 更新跳表的结点
                znode = zslUpdateScore(zs->zsl,curscore,ele,score);
                /* Note that we did not removed the original element from
                 * the hash table representing the sorted set, so we just
                 * update the score. */
				// 更新哈表元素的权重
                dictGetVal(de) = &znode->score; /* Update score ptr. */
                *flags |= ZADD_UPDATED;
            }
            return 1;
        } else if (!xx) {
			// 不存在
            ele = sdsdup(ele);
			// 插入跳表
            znode = zslInsert(zs->zsl,score,ele);
			// 插入 哈希表
            serverAssert(dictAdd(zs->dict,ele,&znode->score) == DICT_OK);
            *flags |= ZADD_ADDED;
            if (newscore) *newscore = score;
            return 1;
        } else {
            *flags |= ZADD_NOP;
            return 1;
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }
    return 0; /* Never reached. */
}
```

