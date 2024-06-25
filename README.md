# EJFAT (ESnet-JLab FPGA Accelerated Transport)

## Check out ejfat, esnet2 branch

    git clone https://github.com/JeffersonLab/ejfat/tree/esnet2
            or 
    gh repo clone JeffersonLab/ejfat
    git checkout esnet2
    
    cd ejfat
    mkdir build
    cd build


    
## -------------------------------------------------------------
## Setting environmental variables

The environmental variable **EJFAT_ERSAP_INSTALL_DIR** must be set.
Either that or **-DINSTALL_DIR** must be specified on the cmake command
which takes precedence over the environmental variable.
This allows for needed libraries and headers to be found. It also
allows for generated files to be stored there.

    export EJFAT_ERSAP_INSTALL_DIR=/daqfs/ersap/installation
        or
    cmake .. -DINSTALL_DIR=/daqfs/ersap/installation
    

## -------------------------------------------------------------
## Latest code with no dependencies


This builds the no-dependency code:

    export EJFAT_ERSAP_INSTALL_DIR=/daqfs/ersap/installation
    cmake ..
        or
    cmake .. -DINSTALL_DIR=/daqfs/ersap/installation
    
    make install
    
From the **simulation** directory, the standard data sender
will always be compiled.
It takes the URI (more on this later) and can parse it to find out where to
send data and sync messages. The following is created:

- **packetBlaster** (packetizes and sends data)



## -------------------------------------------------------------
## Older code with no dependencies & a static Control Plane

    cmake .. -DBUILD_OLD=1
    make

The **emulation** directory, containing Mike Goodrich's old code,
has receivers only configured to use the old, static CP (no grpc).
The following programs will be created:

- **lb_emu**     (emulates LB)
- **pktzr_md**   (packetizes and sends)
- **lb_rcv**     (reassembles to stdout)
- **lb_rcv_roc** (reassembles to file)
- **lb_send**    (sends to LB)

The **staticLB** directory, contains old code with
receivers only configured to use the old, static CP (no grpc).
The following programs will be created:

- **packetBlastee** (receives and reassembles data with static LB only)
- **packetBlasterOld** (sends data to LB or directly to receiver)
- **packetBlasterSmall** (For Debugging, sends really small packets to LB)
- **packetAnalyzer** (For Debugging, receives pkts and prints RE and LB headers)
- **udp_send_order** (sends file, which is read or piped-in, to udp_rcv_order)
- **udp_rcv_order** (receives file sent by udp_send_order and reconstructs it)



## -------------------------------------------------------------
## Latest "simple" API code

### Code talking to Load Balancer (LB) and Control Plane (CP)

##### Direct LB/CP use

The **simpleAPI** directory contains classes **EjfatConsumer** and **EjfatProducer**
which are held in a library along with a couple of executables based on them.
They support the latest CP/LB which requires an LB reservation. This reservation
produces a URI which allows both a producers and consumers to use the CP/LB.
This API was designed to be as simple as possible, hiding much of the complexity
from users. It depends on the boost, protobuf, and grpc-based libs.

Cmake **should** find boost.  
The following grpc-related libs are also necessary:

- libgrpc++.so
- libgrpc++_reflection.so
- libprotobuf.so

In addition, there is a library of ejfat commands for talking grpc to the CP:

- libejfat_grpc.so


##### Simple server

In addition to simple consumers and producers that talk to the LB and CP, there
is a simple server which acts as a broker between users of EJFAT and the LB/CP.
This is designed so that users need to know nothing about the hardware, gRPC,
protobufs and anything else complicated. Once the server has been compiled and
is running, users can communicate with the server - not needing to know the
underlying system.

There are 2 classes, **serverConsumer** and **serverProducer** which contain
all needed functionality. These 2 classes are used in example EJFAT consumers
and producers, **simpleServerConsumer** and **simpleServerSender**,
which use the simple server.


### Find existing external libs if any

If the mentioned libs have been already installed on your system, cmake can
find them by setting an environmental variable to the location of all 4 libs.

    # first look here
    export EJFAT_ERSAP_INSTALL_DIR=<installation dir>
    # then here
    export GRPC_INSTALL_DIR=<installation dir>
        

### Install dependencies if necessary

If these libs are **not** already installed, then more work needs to be done.  

#### 1) install the protobuf package

On ubuntu

    sudo apt install protobuf-compiler
    sudo apt install libprotobuf-dev

On Mac

    brew install protobuf

#### 2) install the grpc package

On ubuntu

    sudo apt search grpc
    sudo apt install libgrpc-dev

On Mac

    brew install grpc

Or download gRPC directly from the official website: https://grpc.io

Or the source code for gRPC is hosted on GitHub:

    git clone https://github.com/grpc/grpc.git


#### 3) install the ejfat_grpc library

Find the package at

    https://github.com/JeffersonLab/ersap-grpc


Follow the instructions to build it

    mkdir -p cmake/build
    cd cmake/build
    cmake ../.. -DINSTALL_DIR=<installation dir>
    make install


### Build the simple API lib and example programs

    cmake .. -DBUILD_SIMPLE=1 -DINSTALL_DIR=<installation dir>
    make install

If the ejfat_grpc lib cannot be found in INSTALL_DIR, cmake will look in
EJFAT_ERSAP_INSTALL_DIR and then in GRPC_INSTALL_DIR

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


### Build apps that talk to the simple server

The applications that talk to the simple server,
**simpleServerSender** and **simpleServerConsumer**,
are automatically built along with the
rest of the code in the simpleAPI directory.
However, listed below are the commands to build them by hand.
This may be useful in incorporating them, or your own versions of them,
into your own build system.


##### Compiling by hand

The code uses classes in the **boost/lockfree/queue.hpp** header file. These boost classes
are part of a header-only library. Thus, no boost libs need to be linked against.

On linux, boost headers are located in /usr/include and thus -I does not need to be
explicitly set.

On my Mac laptop the boost include directory is /usr/local/Cellar/boost/1.72.0_3/include.
This needs to be explicitly set with -I .

    cd simpleAPI
    
    c++ -std=c++14 -I ./ -I ../simulation -I <boost_include_dir> -O3 -o serverConsumer.o -c serverConsumer.cpp
    c++ -std=c++14 -I ./ -I ../simulation -I <boost_include_dir> -O3 -o serverProducer.o -c serverProducer.cpp
    c++ -std=c++14 -I ./ -I ../simulation -I <boost_include_dir> -O3 -o simpleServerSender.o -c simpleServerSender.cpp
    c++ -std=c++14 -I ./ -I ../simulation -I <boost_include_dir> -O3 -o simpleServerConsumer.o -c simpleServerConsumer.cpp
    
    c++ -std=c++14 -O3 -DNDEBUG  simpleServerSender.o serverProducer.o -o simpleServerSender -lpthread
    c++ -std=c++14 -O3 -DNDEBUG  simpleServerConsumer.o serverConsumer.o -o simpleServerConsumer -lpthread


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
    

#### More details

For more details, look at [README.md](./simpleAPI/README.md)


### -------------------------------------------------------------
### -------------------------------------------------------------

### Generating doxygen documentation:

        make doxygen
        
Point your browser to:

        docs/doxygen/html/index.html



### -------------------------------------------------------------
### Where to look for libs & headers

Be sure to define the following environmental variables, depending on what
you're compiling so that cmake know where to find it:

- **et**

        export EJFAT_ERSAP_INSTALL_DIR=<installation dir>
        
- **protobuf**

        # It's not clear exactly where these lib(s) are.
        # The CMakeLists.txt file calls find_package(Protobuf CONFIG REQUIRED)
        # Looks for protobuf-config.cmake file created by Protobuf's installation.
        # This lib is necessary for successfully finding grpc.
        
- **grpc**

        # It's not clear exactly where these libs are.
        # The CMakeLists.txt file calls find_package(gRPC CONFIG REQUIRED)
        # Looks for gRPCConfig.cmake file created by gRPC's installation.

        # If this is unsuccessful, then the grpc libs are looked for in:
        export GRPC_INSTALL_DIR=<installation dir>
        # then here
        export EJFAT_ERSAP_INSTALL_DIR=<installation dir>
        
        # If this is unsuccessful, then CMakeLists.txt file calls
        find_package(gRPC REQUIRED CONFIG PATHS ${GRPC_INSTALL_DR}/lib/cmake/grpc)

        
- **ejfat_grpc**

        # first look here
        export EJFAT_ERSAP_INSTALL_DIR=<installation dir>
        # then here
        export GRPC_INSTALL_DIR=<installation dir>
        
- **hipo**, **lz4**

        # first look here
        export EJFAT_ERSAP_INSTALL_DIR=<installation dir>
        # then here
        export HIPO_HOME=<installation dir>

- **disruptor**

        # first look here
        export EJFAT_ERSAP_INSTALL_DIR=<installation dir>
        # then here
        export DISRUPTOR_CPP_HOME=<disruptor dir>


- **crow**

        # look here
        export CROW_INSTALL_DIR=<installation dir>


- **asio**

        # look here
        export ASIO_INSTALL_DIR=<installation dir>


- **zeroMQ**

        # first look here
        export ZeroMQ_DIR=<zmq dir>
        # then here
        export PC_LIBZMQ_LIBDIR=<>
        # then here
        export PC_LIBZMQ_LIBRARY_DIRS=<>
        # then here
        /usr/lib/x86_64-linux-gnu



### -------------------------------------------------------------
#### Compile old code that used static LB

    cmake .. -DBUILD_OLD=1
    make


### -------------------------------------------------------------
#### Compile non-ET/ERSAP reassembly backends

    cmake .. -DBUILD_BACKEND=1
    make


### -------------------------------------------------------------
#### Compile clasBlaster and clasBlasterNTP

    export HIPO_HOME=<location of HIPO package>
    cmake .. -DBUILD_CLAS=1
    make


### -------------------------------------------------------------
#### Compile simple API

    cmake .. -DBUILD_SIMPLE=1
    make


### -------------------------------------------------------------
#### Compile ERSAP (engines are linux only)

    export ASIO_INSTALL_DIR=<location of asio directory>
    export CROW_INSTALL_DIR=<location of Crow directory>
    cmake .. -DBUILD_ERSAP=1
    make


### -------------------------------------------------------------
### Installation of headers, libs, and executables:

        export EJFAT_ERSAP_INSTALL_DIR=<installation dir>
        cmake ..
            or
        cmake .. -DINSTALL_DIR=<installation dir>
        
        make install


### -------------------------------------------------------------
### Uninstalling headers, libs, and executables:
   
        make uninstall


### -------------------------------------------------------------
### Where to find dependencies

- **et**  at  https://github.com/JeffersonLab/et
- **ejfat-grpc**  at  https://github.com/JeffersonLab/ersap-grpc
- **Disruptor** at https://github.com/JeffersonLab/Disruptor-cpp
- **boost** (commonly available)
- **protobuf** (commonly available)
- **Crow** at https://github.com/CrowCpp/Crow.git  and https://crowcpp.org/master/
- **asio** at https://think-async.com/Asio/


### -------------------------------------------------------------
### How to install the easy packages

#### 1) install the protobuf package

On ubuntu

    sudo apt install protobuf-compiler
    sudo apt install libprotobuf-dev

On Mac

    brew install protobuf

#### 2) install the grpc package

On ubuntu

    sudo apt search grpc
    sudo apt install libgrpc-dev

On Mac

    brew install grpc

Or download gRPC directly from the official website: https://grpc.io

Or the source code for gRPC is hosted on GitHub:

    git clone https://github.com/grpc/grpc.git


#### 3) install the ejfat_grpc library

Find the package at

    https://github.com/JeffersonLab/ersap-grpc


Follow the instructions to build it

    mkdir -p cmake/build
    cd cmake/build
    cmake ../.. -DINSTALL_DIR=<installation dir>
    make install



### -------------------------------------------------------------
### Libraries that may need to be built:

#### From ET

- libet.so
- libet_jni.so
- libet_remote.so

#### From Disruptor

- libdisruptor-cpp.so

#### For ersap-cpp, clas_blaster

- libzmq.so
- libhipo.so
- liblz4.so

#### From grpc

- libgrpc++.so
- libgrpc++_reflection.so
- libprotobuf.so

#### From ejfat's grpc

- libejfat-grpc.so

#### Other necessary libs

- boost libraries

#### Header only code

- Crow
- asio


**Note: not all executables need all libs.
Look into CMakeLists.txt or the README.md file in a specific
directory for more details.**



### -------------------------------------------------------------
### Building clas12 data sender in java
Do the following:

    ant jar

This will create jbuild/lib/ejfat-1.0.jar containing the 
Clas12DataSender class. To run this and get its help output:

    java -cp jbuild/lib/ejfat-1.0.jar org.jlab.epsci.ejfat.clas12source.Clas12DataSender -h



### -------------------------------------------------------------
### Using the ET system as a FIFO for EJFAT backend

#### Create and run the ET system for ejfat reassembler by calling

- et_start_fifo -v -f \<filename\> -s \<event size\> -n \<events/fifo-entry\> -e \<fifo-entries\>

##### Use -h flag to see more options. The events/fifo  >= number of data sources.


- run ersap_et_consumer or copy and modify ersap_et_consumer.cc to
  consume ET events.
   