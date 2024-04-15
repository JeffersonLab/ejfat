# EJFAT
**ESnet-JLab FPGA Accelerated Transport**

## Check out ejfat, esnet branch

    git clone https://github.com/JeffersonLab/ejfat.git
            or 
    gh repo clone JeffersonLab/ejfat
    git checkout esnet
    

## Building ejfat

Start by doing:

    mkdir build
    cd build
    
At this point, the user will need to decide what exactly needs to be compiled.

#### Compiling executables with no external dependencies and a ***static*** Control Plane

    cmake ..
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
- **packetBlasterSmall** (For Debugging, sends really small packets to LB)
- **packetAnalyzer** (For Debugging, receives pkts and prints RE and LB headers)
- **udp_send_order** (sends file, which is read or piped-in, to udp_rcv_order)
- **udp_rcv_order** (receives file sent by udp_send_order and reconstructs it)


These executables will be created with every **make** no matter
which cmake flags are used.


From the **simulation** directory, the following will be created:

- **packetBlaster** (packetizes and sends data)


The **simpleAPI** directory contains classes EjfatConsumer and EjfatProducer
which are held in a library along with a couple of executables based on them.
They support the latest CP/LB which requires an LB reservation and a
specially-generated URI from the reservation which allows both a producer and
consumer of data to use the CP/LB. This API was designed to be as simple
as possible, hiding much of the complexity from users. It depends only on the
boost and grpc-based libs.+
The following will be created:

- **libejfat_simple.so** (consumer and producer C++ classes)
- **simpleConsumer** (receives and reassembles data)
- **simpleProduer** (packetizes and sends data)





#### Where to look for libs & headers

Be sure to define the following environmental variables, depending on what
you're compiling:

- **et**

        export EJFAT_ERSAP_INSTALL_DIR=<installation dir>
        
- **grpc**, **protobuf**

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
        
        
- **zeroMQ**

        # first look here
        export ZeroMQ_DIR=<zmq dir>
        # then here
        export PC_LIBZMQ_LIBDIR=<>
        # then here
        export PC_LIBZMQ_LIBRARY_DIRS=<>
        # then here
        /usr/lib/x86_64-linux-gnu
        


#### Compiling everything

    cmake -DBUILD_ET=1 -DBUILD_ERSAP=1 -DBUILD_DIS=1 -DBUILD_GRPC=1 -DBUILD_CLAS=1 ..
    make



#### Compiling disruptor-related utility libs
Creating **libejfat_util.so** and **libejfat_util_st.a**.

    cmake -DBUILD_DIS=1 ..
    make



#### Compiling everything in simulation and util directories

Among other things, this will make all grpc-enabled reassembly code.

    cmake -DBUILD_DIS=1 -DBUILD_GRPC=1 -DBUILD_ET=1 ..
    make



#### Compiling clas_blaster

    cmake -DBUILD_CLAS=1 ..
    make


#### Compiling ERSAP engines

    cmake -DBUILD_ERSAP=1 -DBUILD_ET=1 ..
    make




## Installation of headers, libs, and executables:

- Do
    
        cmake -DINSTALL_DIR=<dir> ..
        make install

- If INSTALL_DIR is not set as a flag,
  Define the **EJFAT_ERSAP_INSTALL_DIR** environmental variable, and do
   
        make install



## Uninstalling headers, libs, and executables:
   
        make uninstall


## Where to find dependencies

- **et**  at  https://github.com/JeffersonLab/et
- **ejfat-grpc**  at  https://github.com/JeffersonLab/ersap-grpc
- **Disruptor** at https://github.com/JeffersonLab/Disruptor-cpp
- **boost** (commonly available)
- **protobuf** can be obtained as follows:

On ubuntu

    sudo apt install protobuf-compiler
    sudo apt install libprotobuf-dev

- **grpc** can be obtained as follows:


On ubuntu

    sudo apt search grpc
    sudo apt install libgrpc-dev


Download gRPC directly from the official website: https://grpc.io

The source code for gRPC is hosted on GitHub:

    git clone https://github.com/grpc/grpc.git



### List of libraries that may be needed to build:

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


**Note: not all executables need all libs.
Look into CMakeLists.txt or the README.md file in a specific
directory for more details.**



## Building clas12 data sender in java
Do the following:

    ant jar

This will create jbuild/lib/ejfat-1.0.jar containing the 
Clas12DataSender class. To run this and get its help output:

    java -cp jbuild/lib/ejfat-1.0.jar org.jlab.epsci.ejfat.clas12source.Clas12DataSender -h



## Using the ET system as a FIFO for EJFAT backend

#### Create and run the ET system for ejfat reassembler by calling

- et_start_fifo -v -f \<filename\> -s \<event size\> -n \<events/fifo-entry\> -e \<fifo-entries\>

##### Use -h flag to see more options. The events/fifo  >= number of data sources.


- run ersap_et_consumer or copy and modify ersap_et_consumer.cc to
  consume ET events.
   