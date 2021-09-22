### 压缩列表
List、Hash 和 Sorted Set 这三种数据类型，都可以使用压缩列表来保存数据。


zipList 数据结构在内存中的布局，就是一块连续的内存空间，这块空间的起始部分是大小固定的 10 字节元数据，其中记录了 zipList 的总字节数、最后一个元素
的偏移量以及列表元数数量，而这 10 字节后面的内存空间则保存了实际的列表数据。在 zipList 的最后部分，是一个 1 字节的标识（固定 255），用来表示 zipList
的结束。

虽然 ziplist 通过紧凑的内存布局来保存数据，节省了内存空间，但是 ziplist 也面临着随之而来的两个不足：查找复杂度高和潜在的连锁更新风险。

**查找复杂度高**

在 zipList 中头尾元数据的大小固定，并且在 zipList 头部记录了最后一个元素的位置，所以，当在 zipList 中查找第一个或者最后一个元素时，可以很快找到。

但是当要查找中间元素的时候，zipList 就的从头到尾遍历。当 zipList 中保存了很多元素时，查找数据的时间复杂度就增加了。更坏的情况是，当 zipList 中保存的
是字符串时，在查找某个元素的时候，还需要逐一判断元素的每个字符，这样又进一步增加的复杂度。

因此，在使用 zipList 保存 Hash 或 Sorted Set 数据时，都会在 redis.conf 文件中，通过 hash-max-ziplist-entries 和 zset-max-ziplist-entries 
两个参数，来控制保存在 ziplist 中的元素个数。

**连锁更新**

在 zipList 中使用了一块连续的内存空间来保存数据，所以当插入一个新元素的时候，zipList 就需要计算其所需的空间大小，并申请相应的内存空间。在些操作可以从
zipList 的元素插入函数 __ziplistInsert 中看到。

**__ziplistInsert 插入元素**

__ziplistInsert 函数首先会计算获得当前 zipList 的长度，这个步骤通过 ZIPLIST_BYTES 宏定义基于可以完成。同时，该函数还声明了 reqlen 变量，用于记录插入元素
后所需的新增空间大小。

然后， __ziplistInsert 函数会判断当前要插入的位置是否是列表末尾。如果不是末尾，那么需要获取位于当前元素插入位置的元素 prevlen 和 prevlensize。

在 zipList 中，每个元素都会记录其前一项的长度，也就是 prevlen。然后，为了节省内存开销，zipList 会使用不同的空间记录 prevlen，这个 prevlen 空间大小
就是 prevlensize。

为了保证 zipList 有足够的内存空间，来保存插入元素以及插入位置元素的 prevlen 信息，__ziplistInsert 函数在获得插入位置元素的 prevlen 和 prevlensize 后，紧接着就会计算
插入元素的长度。

一个 zipList 元素包括了 prevlen，encoding 和实际数据 data 三个部分。所以，在计算插入元素的所需空间是，__ziplistInsert 函数也会分别计算这三个部分的长度。

* 第一步，计算实际插入元素长度

这个计算过程和插入元素值整数还是字符串有关。__ziplistInsert 函数首先会调用 zipTryEncoding 函数，这个函数会判断插入元素是否为整数。如果是整数，就按照不同的整数大小，
计算 encoding 和实际数据 data 各自所需的空间。如果是字符串，那么就先把字符串长度记录为所需的新增空间大小。


* 第二步，调用 zipStorePrevEntryLength 函数，将插入位置元素的 prevlen 也计算到所需空间中。

* 第三步，调用 zipStoreEntryEncoding 函数，根据字符串长度，计算相应 encoding 的大小。

到了这一步 __ziplistInsert 函数就已经在 reqlen 变量中，记录了插入元素的 prevlen 长度，encoding 大小，以及数据 data 的长度。因此，插入元素的整体长度就有了，这也是
插入位置元素的 prevlen 所要记录的大小。

* 第四步，调用 zipPrevLenByteDiff 函数，用来判断插入位置元素的 prevlen 和实际所需的 prevlen，这两者间的大小差别。使用 nextdiff 来记录。

这里，如果 nextdiff 大于 0， 就表明插入位置元素的空间不够，就需要新增 nextdiff 大小的空间，以便能保存更新的 prevlen。然后 __ziplistInsert 函数在新增空间时，就会调用 
ziplistResize 函数，来重新分配 ziplist 所需的空间。


