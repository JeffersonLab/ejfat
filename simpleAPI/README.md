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



//---------------------------------------------------------------------



I am using the google.protobuf.Timestamp type. https://protobuf.dev/reference/protobuf/google.protobuf/#timestamp is the documentation, with C++ docs here: https://protobuf.dev/reference/cpp/api-docs/google.protobuf.util.time_util/ 

According to ChatGPT, here's a minimal example of how to create that type in C++:

    #include <iostream>
    #include "google/protobuf/timestamp.pb.h" // Include the Timestamp definition
    #include "google/protobuf/util/time_util.h" // Include the TimeUtil helper functions

    int main() {
        // Initialize the Google Protobuf library.
        // Required for version 3.0 and later.
        GOOGLE_PROTOBUF_VERIFY_VERSION;
    
        // Create a Timestamp object.
        google::protobuf::Timestamp timestamp;
    
        // Get the current time and assign it to the timestamp.
        timestamp = google::protobuf::util::TimeUtil::GetCurrentTime();
    
        // Print the timestamp. The TimeUtil::ToString() method converts the timestamp
        // to a human-readable string in RFC 3339 format.
        std::cout << "Current Timestamp: "
                  << google::protobuf::util::TimeUtil::ToString(timestamp) << std::endl;
    
        // Optional: Clean up the Google Protobuf library's internal data structures.
        // Not strictly necessary for short-lived programs.
        google::protobuf::ShutdownProtobufLibrary();
    
        return 0;
    }

If you have an epoch time already, you can use timestamp.set_seconds() I believe.


//---------------------------------------------------------------------


Hi Carl,

In the latest control plane, there is an updated protobuf. There is a new gRPC GetLoadBalancer for use by the sender to retrieve the dataplane/sync information. Now both the sender and receiver could use a URL in the following format:

    ejfat://[token@]<host>:<port>/lb/<lb_id>

and then the sender could use that to make the GetLoadBalancer RPC to fetch the other details it needs. The user should be able to pass the token another way, it should be optional to include the token in the URL. GetLoadBalancer returns the same response type as ReserveLoadBalancer, except the token cannot be retrieved again, so the user will need a token to make the request. The idea being that the ReserveLoadBalancer will return the data to create that URL like:

    bash~$ ./bin/udplbd grpc-mockclient reserve
    EJFAT_URL="ejfat://redactedtoken@192.0.0.1:18347/lb/5"


then other programs will be able to use that EJFAT_URL environment variable to establish their connections, the receiver needs only the information in the URL, while the sender will need to fetch the other details.

Thanks,
Derek


//---------------------------------------------------------------------



Carl, Mike, and Vardan,

I forgot to mention another difference between the new control plane and old - the control signals still affect the relative priority in an additive way like we changed while we were working on the simulation paper, but everything has been scaled by 1000, so your Kp, Ki, Kd should be multiplied by 1000. 

This change is mostly aesthetic. I felt the decimal tunings made more sense for the multiplicative scheme, where a control signal of -0.5 would represent "send me half the work", but since we found that the adding/subtracting relative priority resulted in more stable behavior and this is no longer the case, I feel basing everything around 1000 gives us a nice amount of precision without needing to use decimals in our tunings compared to using 1. 

So tl;dr, if you have a good tuning -Kp 0.1 -Ki 0.01 -Kd 0.05 you would now use -Kp 100 -Ki 10 -Kd 50 when running against the new control plane.


//---------------------------------------------------------------------


Hi Derek,
I need some help sorting this out. This is what I think you're saying.

We write a program to reserve an LB. This program will look something like:

    lb_reserve --name <random_name> --host <cp_host> --port <cp_port> --until "2023-12-31T23:59:60Z" <ADMIN_token>

It will call ReserveLoadBalancerRequest and get back the ReserveLoadBalancerReply. This reply will contain the lb_id of the reserved LB along with the instance token. So now this program creates an environmental variable containing a URL like:

    EJFAT_URI= ejfat://<INSTANCE_token>@<cp_host>:<cp_port>/lb/<lb_id>

This program actually does nothing with the data and sync IP addrs and ports it receives.


      2) The sender can parse this URL to be able to send the GetLoadBalancerRequest command, get back the ReserveLoadBalancerReply, then send sync and data properly. It will, however, need to get the ADMIN token separately.

      3) The receiver can also parse this to be able to communicate with the CP. It should have everything at that point.

Did I get this right? Or am I missing something?
Thanks,
Carl


Hi Carl,

That is mostly correct, but the sender does not need to admin token,
the instance token will be able to call GetLoadBalancer,
though I may have accidentially not allowed that. Will fix.
You also bring up a good point regarding "This program actually does
nothing with the data and sync IP addrs and ports it receives.",
if there's a better way to encode that in a single string,
we could probably just put everything in the EJFAT_URL or equivalent. 

Thanks,
Derek