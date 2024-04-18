## Using the "simple" API for using EJFAT

### **************************************************************
### Building

  Compile the code according to the instructions in the top level
  [README.md](../README.md)



### **************************************************************
### General Info

  #### Senders and Receivers

There are 2 C++ classes to use, one for sending data and the other for receiving:

 - EjfatProducer.cpp/.h
 - EjfatConsumer.cpp/.h
 
 
 There are examples of how to use each in the files:
 
  - simpleSender.cpp
  - simpleConsumer.cpp
  
  All the complexities of interacting with the Load Balancer and the Control Plane
  are hidden from the user. These classes also provide output in terms of
  relevant statistics.
  
  The consumer is able to accept input from multiple senders with all threading
  hidden from the user.
  
  All files, classes, methods, and members are documented using doxygen.
  
  #### Interacting with the Load Balancer (LB) & Control Plane (CP)

There are 3 programs used to talk to the CP:

 - lbreserve
 - lbfree
 - lbmonitor
 
 **lbreserve** reserves an instance of a LB  
 **lbfree** releases a reserved LB  
 **lbmonitor** periodically prints the status of registered users of a CP
 
### **************************************************************
### Steps to running
 
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
 
 Note: the amount of time an LB is reserved can be set on the command line.
 Currently, even though the reservation is for 10 minutes, it will actually
 last forever.
 
 #### 2) Run a data sender
 
 An example of a data sender is the program simpleSender.cpp.
 Of course, it just sends nonsense data and is only useful as an example and
 for testing data transfer rates.  
 
 To send meaningful data, modify that program or use the EjfatProducer class
 directly. Doxygen documentation of this class is available if the
 **"make doxygen"** command was executed.
 
 
    // The simplest implementation is:
        
         
        
 #### 3) Run a data consumer
 
 
 




 ### **************************************************************
 ### Steps to XXXXXX

  

