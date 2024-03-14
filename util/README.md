## Where to find Disruptor

Find Jefferson Lab's fork of the C++ **Disruptor** at https://github.com/JeffersonLab/Disruptor-cpp

A bit of history. The prize-winning Disruptor software was originally written in Java.
A Jefferson Lab fork of that package is at
https://github.com/JeffersonLab/disruptor .
It's well worth following the documentation links in order to understand
how it all works. The C++ disruptor is just a port of the Java.
All completely threadsafe.

This fills strangly missing niche in C++ in which there are no
blocking queues that are part of the language!??


### Files in this directory

These files are compiled into shared and static libraries,
**libejfat_util.so** and **libejfat_util_st.a**.
These libraries implement an ultra-fast ring buffer based on the
Disruptor library.



#### ByteOrder.cpp/.h

These 2 files provide a class which is a description of byte order
and has methods to do swapping. It's roughly parallel a Java class of the
same name.

#### ByteBuffer.cpp/.h
These 2 files implement a C++ class which is a port of a Java class by the same name.
It's an extremely useful class and having a C++ version is very convenient.
Basically, it represent a buffer of data with many methods to manipulate it.
This class uses the ByteOrder class.

#### BufferSupply.cpp/.h & BufferItem.cpp/.h

These 2 classes were originally written in Java for the CODA data acquistion
system. Being so fast, effective and useful, they are ported to C++ here.
It is used to provide a very fast supply of ByteBuffer objects
(actually ByteBufferItem objects each of which wraps a ByteBuffer)
for reuse in 2 different modes (using the Disruptor software package).<p>

1) It can be used as a simple supply of ByteBuffer(Item)s.
In this mode, only get() and release() are called. A user does a supply.get(),
uses that buffer, then calls supply.release() when done with it. If there are
multiple users of a single buffer (say 5), then call bufferItem.setUsers(5)
before it is used and the buffer is only released when all 5 users have
called release().<p>

2) It can be used as a supply of ByteBuffers in which a single
producer provides data for a single consumer which is waiting for that data.
The producer does a supply.get(), fills the buffer with data,
and finally does a supply.publish()
to let the consumer know the data is ready. Simultaneously, a consumer does a
supply.consumerGet() to access the data buffer once it is ready. To preserve all the data
as the producer wrote it, use the item.getBuffer() when getting
the data buffer. The consumer then calls supply.release() when finished
which allows the producer to reuse the now unused buffer.<p>


#### Supplier.h, SupplierN.h, SupplyItem.cpp/.h
These classes are a templated generalization of
BufferSupply and BufferItem. Thus, Supplier(N) classes are generalizations
of BufferSupply and can supply items which inherit from the SupplyItem
class (a generalization of BufferItem) and extend it.

The Supplier classes become a ring supplier of items that can have many
different functions and not just contain a byte buffer.

The difference between Supplier and SupplierN is that Supplier works
with 1 producer and 1 consumer whereas SupplierN can have multiple (N) consumers.
The number of clients must be set in the constructor. It is completely up to
the N consumers as to any orchestration needed over who does what to which
ring entries. For example, with 2 consumers, one may operate on all "odd" items
and the other on "even" items. This, of course, depends on the type of items
being supplied.


#### PacketRefItem.cpp/.h  PacketsItem.cpp/.h  BufferItem.cpp/.h  PacketsStoreItem.cpp/.h

These classes are used as objects to be supplied by Supplier.
- **BufferItem** holds a ByteBuffer and essentially turns the Supplier into a BufferSupply.
- **PacketsItem** is designed to work with the linux routine, recvmmsg,
  and stores the many packets obtained by it along with their RE headers.
  This is not used any more as recvmmsg turned out to be a slower way to
  read packets from a socket.
- **PacketStoreItem** holds a single UDP packet along with its RE header.
- **PacketRefItem** defines a place to store a PacketStoreItem and the Supplier it came from.
  This holds them temporarily until the packet is used to reconstruct a complete event buffer,
  then the packet can be released back to the supply and eventually reused.
  Uses shared pointers and thus avoids copying packet data.


#### PacketsItemN.cpp/.h
Not used anymore but plays the role of PacketsItem described above but
for SupplierN. Keep this as an example.