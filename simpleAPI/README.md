Hi Carl,

Regarding the simple EJFAT C API, I think that the API in https://github.com/JeffersonLab/ejfat/tree/esnet/simulation already contains the building blocks to create this simple API, and creating a higher-level API would primarily involve (1) refactoring the packetBlasteeFullNewMP and packetBlaster programs into a library, so that the user does not need worry about the gRPC or event number sync themselves. Essentially everything that is currently an CLI argument would become a function parameter (2) creating some wrappers around those functions that would abstract away many of the details as we can with some good defaults, and (3) implementing a new gRPC that would allow the sender/reciever to retrieve the LB details (probably just the ReserveLoadBalancerReply), which would be called via the details in a new EJFAT URI scheme, and using this in the library threads to retrieve the details needed.

For the highest-level API, I think that the threads for the sync messages and control signals would be spun up on creation of the sender/receiver, and the system clock could be used to generate the event numbers if these are not provided, so the user doesn't have to worry about anything but their buffers, and I think we could get it to where it would pretty much feel like POSIX send on the sending side, and just like popping events of a queue on the receiving side. 

First, the user would call a CLI that we would develop to reserve the LB and get a URI:

    bash~$ ejfat reserve --name example --until "2023-12-31T23:59:60Z" ejfat-control-plane.es.net
    EJFAT_URI=ejfat://abcdef123456789@ejfat-control-plane.es.net:12345/lb/1

The user could then pass that as an environment variable to both the sender and reciever:

Sending side:

    // launches thread that sends sync messages, struct contains socket needed to send UDP packets
    EJFATProducer* producer = ejfat_producer_new(getenv("EJFAT_URI"));
    
    while (true) {
        // user gets their data some arbitrary way
        UserData* data = user_recieve_data();
        // system time used for event number if event_id is 0
        // blocks until at least one node is registered.
        int err = ejfat_send(producer, data->buffer, data->size, data->event_id);
    }

Receiving side:

    EJFATConsumer* consumer = ejfat_consumer_new(getenv("EJFAT_URI"), "203.0.113.1", 20000, 100., 0., 0.); // (uri, name, listen addr, listen port, Kp, Ki, Kd). launches a thread that listens and reassembles packets and puts events in a shared queue. Also launches a thread or uses an event loop to handle the registration and status updates. Automatically determines node name via hostname, weight=1,  
    
    
    while (true) {
        EJFATEvent* event = ejfat_recv(consumer, 0); // second parameter is timeout. blocks until there is an event to pop off the shared queue or times out. 
        if (event) {
            process_event(event);
            ejfat_free_event(event);
        } else {
            if (consumer->error) {
                fprintf(stderr, "error recieving data: %s\n", consumer->error);
                exit(1);
            } else { // load balancer freed, no more events
                break
            }
        }
    }

Feel free to change anything about this, I am not attached to any of these ideas. The main idea I am trying to get across is that I think with just one additional gRPC to retrieve the LB details again after reservation, I believe we will be able to reduce the setup for an EJFAT workflow to a couple of key functions, where the API for the sender mostly just involves passing a URI and then passing some buffers, and the API for the receiver mostly just involves passing a URI, the PID tuning parameters, and then pulling events off the shared queue. I am not an expert at C/C++ so there are probably better conventions for error handling and passing the buffers around. Also, I could see users wanting to spin up the threads themselves, but I think it's key that there would be a way to use library-provided threads that can handle all of the gRPC and event number sync interactions without the user needing to explicitly call into these - that is, I think high-level API boundary should be just passing a URI and essential config that cannot be defaulted, then just sending and receiving the data; that is, I think the FIFO/control loop and event number sync should be (optionally) provided by the library.

Please let me know what you think or if there is anything I should clarify. 

Thanks,
Derek