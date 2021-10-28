#ifndef STREAM_H
#define STREAM_H

#include "rax.h"
#include "listpack.h"

/* Stream item ID: a 128 bit number composed of a milliseconds time and
 * a sequence counter. IDs generated in the same millisecond (or in a past
 * millisecond if the clock jumped backward) will use the millisecond time
 * of the latest generated ID and an incremented sequence. */
typedef struct streamID {
    // 消息的创建时间
    uint64_t ms;        /* Unix time in milliseconds. */
    // 序号
    uint64_t seq;       /* Sequence number. */
} streamID;

typedef struct stream {
    // 保存消息的 Radix Tree
    rax *rax;               /* The radix tree holding the stream. */
    // 消息流中的消息个数
    uint64_t length;        /* Number of elements inside this stream. */
    // 当前消息流中最后插入的消息的ID
    streamID last_id;       /* Zero if there are yet no items. */
    // 当前消息的消费组信息，也是用 Radix Tree 保存
    rax *cgroups;           /* Consumer groups dictionary: name -> streamCG */
} stream;

/* We define an iterator to iterate stream items in an abstract way, without
 * caring about the radix tree + listpack representation. Technically speaking
 * the iterator is only used inside streamReplyWithRange(), so could just
 * be implemented inside the function, but practically there is the AOF
 * rewriting code that also needs to iterate the stream to emit the XADD
 * commands. */
typedef struct streamIterator {
    // 当前迭代器正在遍历的消息流
    stream *stream;         /* The stream we are iterating. */
    // listpack 中第一个插入的消息ID
    streamID master_id;     /* ID of the master entry at listpack head. */
    // 第一个 entry 的 field 个数
    uint64_t master_fields_count;       /* Master entries # of fields. */
    // master entry 的 field 首地址
    unsigned char *master_fields_start; /* Master entries start in listpack. */
    // 记录 field 的位置
    unsigned char *master_fields_ptr;   /* Master field to emit next. */
    // 当前遍历消息的标志位
    int entry_flags;                    /* Flags of entry we are emitting. */
    // 迭代器方向
    int rev;                /* True if iterating end to start (reverse). */
    // 遍历范围
    uint64_t start_key[2];  /* Start key as 128 bit big endian. */
    uint64_t end_key[2];    /* End key as 128 bit big endian. */
    // rax 迭代器，用于遍历 rax 树中的所有 key
    raxIterator ri;         /* Rax iterator. */
    // 当前 listspack 指针
    unsigned char *lp;      /* Current listpack. */
    // 当前正在遍历的 listspack 中的元素
    unsigned char *lp_ele;  /* Current listpack cursor. */
    unsigned char *lp_flags; /* Current entry flags pointer. */
    /* Buffers used to hold the string of lpGet() when the element is
     * integer encoded, so that there is no string representation of the
     * element inside the listpack itself. */
    // 从 listpack 读取数据的缓存
    unsigned char field_buf[LP_INTBUF_SIZE];
    unsigned char value_buf[LP_INTBUF_SIZE];
} streamIterator;

/* Consumer group. */
/** 消费组，每个 stream 会有多个消费组*/
typedef struct streamCG {
    // 该消费组已经确认的最后一个消息Id
    streamID last_id;       /* Last delivered (not acknowledged) ID for this
                               group. Consumers that will just ask for more
                               messages will served with IDs > than this. */
    // 该消费组尚未确认的消息，消息 ID 为键，streamNACK 为值，存储在 rax 树中
    rax *pel;               /* Pending entries list. This is a radix tree that
                               has every message delivered to consumers (without
                               the NOACK option) that was yet not acknowledged
                               as processed. The key of the radix tree is the
                               ID as a 64 bit big endian number, while the
                               associated value is a streamNACK structure.*/
    // 消费组中的所有消费者，消费者名称为键，streamConsumer 为值
    rax *consumers;         /* A radix tree representing the consumers by name
                               and their associated representation in the form
                               of streamConsumer structures. */
} streamCG;

/* A specific consumer in a consumer group.  */
/** 消费组中的消费者*/
typedef struct streamConsumer {
    // 该消费者左后一次活跃时间
    mstime_t seen_time;         /* Last time this consumer was active. */
    // 消费者名称
    sds name;                   /* Consumer name. This is how the consumer
                                   will be identified in the consumer group
                                   protocol. Case sensitive. */
    // 消费者尚未确认的消息
    rax *pel;                   /* Consumer specific pending entries list: all
                                   the pending messages delivered to this
                                   consumer not yet acknowledged. Keys are
                                   big endian message IDs, while values are
                                   the same streamNACK structure referenced
                                   in the "pel" of the conumser group structure
                                   itself, so the value is shared. */
} streamConsumer;

/* Pending (yet not acknowledged) message in a consumer group. */
/** 未确认的消息，streamNACK 维护了消费组或消费者尚未确认的消息，消费组中的 pel 的元素与消费者的 pel 元素是共享的*/
typedef struct streamNACK {
    // 消息最后发送给消费方的时间
    mstime_t delivery_time;     /* Last time this message was delivered. */
    // 消息发送的次数
    uint64_t delivery_count;    /* Number of times this message was delivered.*/
    // 当前归属的消费者
    streamConsumer *consumer;   /* The consumer this message was delivered to
                                   in the last delivery. */
} streamNACK;

/* Stream propagation informations, passed to functions in order to propagate
 * XCLAIM commands to AOF and slaves. */
typedef struct sreamPropInfo {
    robj *keyname;
    robj *groupname;
} streamPropInfo;

/* Prototypes of exported APIs. */
struct client;

stream *streamNew(void);
void freeStream(stream *s);
size_t streamReplyWithRange(client *c, stream *s, streamID *start, streamID *end, size_t count, int rev, streamCG *group, streamConsumer *consumer, int flags, streamPropInfo *spi);
void streamIteratorStart(streamIterator *si, stream *s, streamID *start, streamID *end, int rev);
int streamIteratorGetID(streamIterator *si, streamID *id, int64_t *numfields);
void streamIteratorGetField(streamIterator *si, unsigned char **fieldptr, unsigned char **valueptr, int64_t *fieldlen, int64_t *valuelen);
void streamIteratorStop(streamIterator *si);
streamCG *streamLookupCG(stream *s, sds groupname);
streamConsumer *streamLookupConsumer(streamCG *cg, sds name, int create);
streamCG *streamCreateCG(stream *s, char *name, size_t namelen, streamID *id);
streamNACK *streamCreateNACK(streamConsumer *consumer);
void streamDecodeID(void *buf, streamID *id);
int streamCompareID(streamID *a, streamID *b);
void streamIncrID(streamID *id);

#endif
