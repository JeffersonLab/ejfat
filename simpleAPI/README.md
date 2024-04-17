## Instructions on using the "simple" API for using EJFAT

### **************************************************************
### Building

  #### Getting the dependencies in place
  
  
  

  #### Compile the simple API library and executables
  
In the top ejfat directory:

    mkdir build  
    cd build  
    cmake .. -DBUILD_SIMPLE=1  
    make  




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

There are 2 programs used to talk to the CP:

 - lbreserve
 - lbfree
 
 The lbreserve program will reserve an instance of a Load Balancer
 while lbfree will release it.
 
### **************************************************************
### Steps to running
 
  #### 1) Reserve a load balancer
  
  To see all command line options:

    lbreserve -h  

  At bare minimum, specify the IP addr of the CP

    lbreserve -host 129.57.177.135
    
  This will reserve the LB for 10 minutes and store the resulting URI
  in the file /tmp/ejfat_uri by default.  
    
  One can capture the resulting URI into an environmental variable (using bash):
    
    export EJFAT_URI=$(lbreserve -name myLB -host 129.57.177.135)  
    env | grep EJFAT
 
 
 #### 2) Run a data sender
 
        Now type stuff  
        
 #### 3) Run a data consumer
 
 
 




 ### **************************************************************
 ### Steps to XXXXXX

  

