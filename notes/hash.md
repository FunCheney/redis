### Hash表

** Redis 如何解决 哈希冲突和 rehash 开销的？**

  * 针对哈希冲突，redis 采用了链式哈希，在不扩容的前提下，将具有相同哈希值的数据链接起来，以便这些数据在表中仍然可以查到。
  * 对于 rehash 开销，redis 实现了渐进式 rehash 设计，进而缓解了 rehash 操作带来的额外开销对系统性能的影响。
