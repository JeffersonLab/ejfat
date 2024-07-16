# Using EJFAT's "simple" APIs

### **************************************************************
## Building

  Compile the code according to the instructions in the top level
  [README.md](../README.md)



### **************************************************************
## General Info

There are 2 types of "simple" APIs in the **simpleAPI** directory
allowing programmatic interaction with EJFAT:

 1) an API which talks directly to network hardware and must link against
    grpc and the ejfat-grpc libraries.
 2) an API which talks to a broker that, in turn, talks to the network hardware.
    Senders and receivers do not link against grpc and ejfat-grpc libs.
    
There are also commands/executables which allow handling of the Control Plane (**CP**).
 
### **************************************************************
 
### Type 1, simple API

There are 2 C++ classes to use, contained in the **libejfat_simple.so** library,
one for sending data and the other for receiving:

 - **EjfatProducer**
 - **EjfatConsumer**
 
 
 There are examples of how to use each in the files:
 
  - simpleSender.cpp
  - simpleConsumer.cpp
  
  which are compiled into the executables, **simpleSender** and **simpleConsumer**.
  All the complexities of interacting with the Load Balancer and the Control Plane
  are hidden from the user. These classes also provide output in terms of
  relevant statistics.
  
  The consumer is able to accept input from multiple senders with all threading
  hidden from the user.
  
  All files, classes, methods, and members are documented using doxygen.
  
  
  
### Type 2, even simpler API
    
  In addition to simple consumers and producers that talk directly to the LB and CP,
  there is a server (**EjfatServer** class in **libejfat_simple.so**) which acts as
  a broker between users of EJFAT and the LB/CP. It's compiled into the **simpleServer**
  executable.
  
  This is designed so that users need to know nothing about the hardware, gRPC,
  protobufs and anything else complicated. Once the server has been compiled and
  is running, users can communicate with the server - not needing to know the
  underlying system.
  
  There are 2 classes, **serverConsumer** and **serverProducer** which contain
  all needed functionality. These 2 classes are used in example EJFAT consumers
  and producers, **simpleServerConsumer** and **simpleServerSender**,
  which use the simple server.
  
### **************************************************************

  ### Interacting with the Control Plane

There are 4 programs, created from this directory, used to talk to the CP:


 **lbreserve** reserves an instance of a LB  
 **lbfree** releases a reserved LB  
 **lbmonitor** periodically prints the status of registered users of a CP  
 **lbgeturi** given a CP and a valid lbid, it returns the URI (minus token).
 
### **************************************************************
### Steps to running example Type 1 sender & consumer
 
  #### 1) Reserve a load balancer
  
  To see all command line options:

    lbreserve -h  

  At a bare minimum, specify the IP addr of the CP:

    lbreserve -host 129.57.177.135
    
  This will reserve an LB for 10 minutes and store the resulting URI
  in the file /tmp/ejfat_uri by default. That URI will contain the LB's name
  and all connection info. 
    
  One can capture the resulting URI into an environmental variable (using bash):
    
    export EJFAT_URI=$(lbreserve -name myLB -host 129.57.177.135)  
    env | grep EJFAT
 
 An easier way to capture the URI is to store it in a file. By default it
 gets stored in /tmp/ejfat_uri, but it can be specified on the command line:
 
     lbreserve -host 129.57.177.135 -file <myfile>

 
 Note: the amount of time an LB is reserved can be set on the command line.
 Currently, even though the reservation is for 10 minutes, it will actually
 last forever.
 
 #### 2) Run a data sender
 
 An example of a data sender is the program simpleSender.cpp.
 Of course, it just sends nonsense data and is only useful as an example and
 for testing data transfer rates. It uses the URI to establish all necessary
 network connections.
 
 To send meaningful data, modify that program or use the EjfatProducer class
 in your own program. Doxygen documentation of this class is available if the
 **"make doxygen"** command was executed
 (see  [README.md](../README.md#generating-doxygen-documentation)). These
 docs explain all constructor parameters, else the programmer can look
 into EjfatProducer.cpp directly.  
 
 One thing to be aware of is that there are 2 ways to send data:  
 
 1) The first method blocks when it sends
 
 2) The second method is nonblocking, places data on an internal queue,
    and a thread sends everything on the queue. If the queue is full,
    false is returned.
 
 
 
 To show a minimal example:
     
    #include "EjfatProducer.h"
    
    // The simplest EjfatSender implementation in which the URI
    // (containing LB/CP contact info) is obtained from the file
    // /tmp/ejfat_uri. Everything uses default settings.
    
    EjfatProducer producer();
    
    // Obtain data
    char data[1] = {1};
    size_t dataSize = 1;
    
    // Blocking send
    producer.sendEvent(data, dataSize);
    
    // Non-blocking send
    bool added = producer.addToSendQueue(data, dataSize);  
    
    

Besides the basic functionality shown, the user has much more flexibility.
In the send commands, an event number can be specified, and for the non-blocking
send, a callback and its arg can be specfied to run after it's been sent. Many
more parameters are available to use in the constructor.  

        
#### 3) Run a data consumer
 
An example of a data receiver is the program simpleConsumer.cpp.
By default it receives all events sent by the simpleSender. Like the sender,
the consumer uses the URI to establish all necessary network connections.

 
To consume data, modify that program or use the EjfatConsumer class
in your own program. Unlike the sender, the consumer needs to specify at least
one arg to the constructor - the IP address on which it wants to receive data.  


Only a non-blocking **getEvent** call is available, although the user can create
their own blocking version of this call. This method returns 4 things:

 - pointer to event's data
 - size of data in bytes
 - number of the event
 - id number of sending source

 
To show a minimal example:
    
    #include "EjfatConsumer.h"
    
    // The simplest EjfatConsumer implementation in which the URI
    // (containing LB/CP contact info) is obtained from the file
    // /tmp/ejfat_uri. Setup for one source whose id = 0.
    
    // address to receive on
    std::string myIP = "129.57.177.5";
    
    EjfatConsumer consumer(myIP);
    
    char*    event;
    size_t   bytes;
    uint16_t srcId;
    uint64_t eventNum;
    
    while (true) {
        // Non-blocking call to get a single event
        bool gotEvent = consumer.getEvent(&event, &bytes, &eventNum, &srcId);
    
        if (gotEvent) {
            // do something with data
        }
        else {
            // Nothing in queue, sleep?
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
    }
   
Besides the basic functionality shown, the user has much more flexibility.
Just to mention one, each consumer can receive from multiple sources, each
in its own thread. To do this, the constructor must be given a vector containing
all expected ids of the sources.  


#### 4) Coordinate between multiple senders and a consumer

Getting multiple senders to a single consumer requires some coordination.
The first order of business is to give the EjfatConsumer constructor a
vector containing all ids of the expected sources. In communication
with the CP, this allows for each sender's events to end up on its own
port on the consumer's host.

On the sender side, the easiest way to accomplish this is to have senders'
id numbers be sequential, starting from 0. Then have each sender set its
entropy to its id#. Thus sender 5 has entropy = 5, sender 3 has entropy = 3,
etc.

Back on the consumer side, this results in sender 0 going to the dataPort
(specified in constructor or default = 17750). Sender 1 goes to 17751,
sender 5 goes to 17755, etc. Basically, a sender's entropy value will add
to the consumer's base port to give the receiving port for that sender.

Under the hood, this greatly simplifies how UDP packets can be read and
reconstructed into events without a painful amount of sorting. In fact,
**this coordination is required**.


#### 5) Cleaning up

After a user is finished using a Load Balancer, it's good to remove it
so others can reserve their own. Do this by running:

    lbfree -host 129.57.177.135 -lbid <id>
    
where id is taken from the URI obtained when reserving.


### **************************************************************


The following will be created:

- **libejfat_simple.so**    (consumer and producer C++ classes)
- **simpleConsumer**        (receives and reassembles data)
- **simpleSender**          (packetizes and sends data)
- **lbreserve**             (reserves a load balancer)
- **lbfree**                (frees a load balancer)
- **lbmonitor**             (prints stats of a load balancer)


- **simpleServer**          (server brokering clients to LB/CP)
- **simpleServerConsumer**  (consumer talking to simple server)
- **simpleServerSender**    (producer talking to simple server)



### How to run the server/broker and the simpleServerConsumer/Producer

The example below is how to run a single server with 1 consumer and
2 producers (ids = 0 and 1).

#### 1) Run the server

    simpleServer -file /tmp/myFileWithUri

where **-file** is file containing the uri to talk to the LB/CP (previously obtained
by calling lbreserve).


#### 2) Run the consumer

    simpleServerConsumer -server 129.57.177.2 -a 129.57.177.4 -ids 0,1

where **-server** is simple server's host, **-a** is IP addr listening
for data from LB, **-ids** are all expected source ids.

#### 3) Run the first producer

    simpleServerSender -server 129.57.177.2 -id 0 -d 10
    
where **-server** is simple server's host, **-id** is src ID,
**-d** is delay in microsec between events

#### 4) Run the second producer

    simpleServerSender -server 129.57.177.2 -id 1 -d 20
    
