### Redis 内存数据结构
&ensp;&ensp;Redis 通过两种方式来提高内存使用效率，分别是①: 数据结构优化设计使用;②：内存数据按照一定规则淘汰。

#### 数据结构优化设计使用
&ensp;&ensp;Redis 针对三种数据结构做了设计优化，分别是简单动态字符串(SDS)、压缩列表(zipList)和整数集合(intset)。

#### redisObject 结构体

```c++
typedef struct redisObject {
    unsigned type:4;  //redisObject的数据类型，4个bits
    unsigned encoding:4; //redisObject的编码类型，4个bits
    //redisObject的LRU时间，LRU_BITS为24个bits
    unsigned lru:LRU_BITS; /* LRU time (relative to global lru_clock) or
                            * LFU data (least significant 8 bits frequency
                            * and most significant 16 bits access time). */
    //redisObject的引用计数，4个字节
    int refcount;
    //指向值的指针，8个字节
    void *ptr;
} robj;
```
* type：redisObject 的数据类型，是应用程序在 Redis 中保存的数据类型，包括 String、List、Hash 等。
* encoding：redisObject 的编码类型，是 Redis 内部实现各种数据类型所用的数据结构。
* lru：redisObject 的 LRU 时间
* refcount： redisObject 的引用计数
* ptr：指向值的指针

&ensp;&ensp;从代码中可以看出，在 type、encoding 和 lru 三个变量后面都有一个冒号，并紧跟着一个数值，表示该元数据占用的比特数。其中 type、
encoding 分别占用 4bits。而 lru 占用的比特数，是由 server.h 中的宏定义 LRU_BITS 决定的，它的默认值是 24bits。


