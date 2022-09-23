package org.jlab.epsci.ejfat.clas12source;


import java.io.*;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.net.*;

import j4np.hipo5.data.Event;
import j4np.hipo5.io.HipoReader;


/**
 * Test program.
 * First used to write a big (6GB) file and now used to test the
 * RAID for hall D.
 *
 * @author timmer
 * Date: Oct 7, 2010
 */
public class Clas12DataSender {

    /** Size of EJFAT load balancing header */
    static final int LB_HEADER_BYTES = 16;
    /** Size of UDP EJFAT reassembly header. */
    static final int RE_HEADER_BYTES = 16;
    /** Size of all headers. */
    static final int HEADER_BYTES = RE_HEADER_BYTES + LB_HEADER_BYTES;
    /** Default MTU to be used if it cannot be found programmatically. */
    static final int DEFAULT_MTU = 1400;



    private boolean debug, random;
    private int bufferBytes = 15000;    // Write data in 15k byte chunks by default
    private int repeat = 3;
    private String filename;

    private int sendBufSize = 25000000; // 50 MB UDP send buffer is default
    private int port = 0x4c42; // FPGA port is default
    private String destHost = "172.19.22.241";


    /** FPGA Load Balancer reassembly protocol. */
    private final int lbProtocol = 1;
    /** FPGA Load Balancer reassembly version. LB only works for version = 2. */
    private final int lbVersion = 2;
    /** VTP reassembly version */
    private final int reVersion = 1;

    private int mtu = DEFAULT_MTU;




// How to set core affinity for java thread
//    Say 2241 is the pid of your java process. Run:
//
//    jstack 2241
//
//    This gives you a list of threads. Find yours there and note the nid field. Say nid=0x8e9, which converts to base 10 as 2281. Then run:
//
//    taskset -p -c 0 2281
//
//            Done.


//    # dladm set-linkprop -p mtu=new-size datalink
//
//    After changing the MTU size, you can reconfigure an IP interface over the datalink.
//
//    The following example shows how to enable support for Jumbo frames. This example assumes that you have already removed any existing IP interface configuration over the datalink.
//
//            # dladm show-linkprop -p mtu net1
//    LINK     PROPERTY        PERM VALUE        EFFECTIVE    DEFAULT   POSSIBLE
//    net1     mtu             rw   1500         1500         1500      1500
//
//            # dladm set-linkprop -p mtu=9000 net1
//# dladm show-link net1
//    LINK     CLASS     MTU      STATE     BRIDGE     OVER
//    web1     phys      9000     up        --         --

    /** Constructor. */
    Clas12DataSender(String[] args) {
        decodeCommandLine(args);
    }


    /**
     * Method to decode the command line used to start this application.
     * @param args command line arguments
     */
    private void decodeCommandLine(String[] args) {

        // loop over all args
        for (int i = 0; i < args.length; i++) {

            if (args[i].equalsIgnoreCase("-h")) {
                usage();
                System.exit(-1);
            }
            else if (args[i].equalsIgnoreCase("-r")) {
                try {
                    repeat = Integer.parseInt(args[i+1]);
                }
                catch (NumberFormatException e) {/* default to 3 */}
                i++;
            }
            else if (args[i].equalsIgnoreCase("-host")) {
                destHost = args[i+1];
                i++;
            }
            else if (args[i].equalsIgnoreCase("-file")) {
                filename = args[i+1];
                i++;
            }
            else if (args[i].equalsIgnoreCase("-sb")) {
                try {
                    sendBufSize = Integer.parseInt(args[i+1]);
                }
                catch (NumberFormatException e) {/* default to 25MB bytes */}
                i++;
            }
            else if (args[i].equalsIgnoreCase("-mtu")) {
                try {
                    mtu = Integer.parseInt(args[i+1]);
                    if (mtu > 900) mtu = 9000;
                    else if (mtu < 1000) mtu = 1000;
                }
                catch (NumberFormatException e) {/* default to 1400 bytes */}
                i++;
            }
            else if (args[i].equalsIgnoreCase("-debug")) {
                debug = true;
            }
            else {
                usage();
                System.exit(-1);
            }
        }

        if (filename == null) {
            usage();
            System.exit(-1);
        }

        return;
    }

    /** Method to print out correct program command line usage. */
    private static void usage() {


//        "        [-h] [-v] [-ip6] [-sendnocp]",
//                "        [-bufdelay] (delay between each buffer, not packet)",
//                "        [-host <destination host (defaults to 127.0.0.1)>]",
//                "        [-p <destination UDP port>]",
//                "        [-i <outgoing interface name (e.g. eth0, currently only used to find MTU)>]",
//                "        [-mtu <desired MTU size>]",
//                "        [-t <tick>]",
//                "        [-ver <version>]",
//                "        [-id <data id>]",
//                "        [-pro <protocol>]",
//                "        [-e <entropy>]",
//                "        [-b <buffer size>]",
//                "        [-bufrate <buffers sent per sec>]",
//                "        [-byterate <bytes sent per sec>]",
//                "        [-s <UDP send buffer size>]",
//                "        [-fifo (Use SCHED_FIFO realtime scheduler for process - linux)]",
//                "        [-rr (Use SCHED_RR realtime scheduler for process - linux)]",
//                "        [-cores <comma-separated list of cores to run on>]",
//                "        [-tpre <tick prescale (1,2, ... tick increment each buffer sent)>]",
//                "        [-dpre <delay prescale (1,2, ... if -d defined, 1 delay for every prescale pkts/bufs)>]",
//                "        [-d <delay in microsec between packets>]");


        System.out.println("\nUsage:\n\n" +
                "   java Clas12DataSender\n" +
                "        [-file <filename>]              name of file to read events from\n" +
                "        [-host <destination host>]      defaults to 172.19.22.241 (ejfat-1 U280)\n" +
                "        [-sb <UDP send buffer size>]     size of UDP send buffer in bytes\n" +
                "        [-r <repeat>]          repeat file read this many times, track avg rate\n" +
                "        [-debug]               turn on printout\n" +
                "        [-h]                   print this help\n");
    }


    /**
     * Run as a stand-alone application.
     */
    public static void main(String[] args) {
        Clas12DataSender sender = new Clas12DataSender(args);
        sender.run();
    }


    /** Read events from file for testing purposes.  */
    public void run() {

        try {
            HipoReader reader = new HipoReader();
            reader.open(filename);

            Event event = new Event();
            int counter = 0;

            while (reader.hasNext()) {
                reader.nextEvent(event);
                int evtLength = event.getEventBufferSize();

                ByteBuffer eventBuffer = event.getEventBuffer();
                eventBuffer.rewind();

                byte[] evt = new byte[evtLength];
                eventBuffer.get(evt);

                counter++;
            }

            System.out.println("File has " + counter + " events");

            int entropy = 0;
            boolean useConnectedSocket = true;
            int maxUdpPayload = 9000;

            // Create UDP socket
            System.out.println("create UDP sending socket");
            DatagramSocket outSocket = new DatagramSocket();
            // Implementation dependent send buffer size
            outSocket.setSendBufferSize(sendBufSize);
            InetAddress destAddr = InetAddress.getByName(destHost);
            //destAddr = InetAddress.getByName("192.168.0.125");
//logger.info("    DataChannel UDP out: connect UDP socket to dest " + destAddr.getHostName() + " port " + port);
            if (useConnectedSocket) {
                outSocket.connect(destAddr, port);
            }

            System.out.println("    DataChannel UDP out: create UDP sending socket with " +
                    " with " + outSocket.getSendBufferSize() + " byte send buffer, on port " +
                    outSocket.getLocalPort() + " to port " + outSocket.getPort() + " to host " + destAddr.getHostName());

        // Break data into multiple packets of MTU size.
        // Attempt to get MTU progamatically.
        int mtu = getMTU(outSocket);
            System.out.println("MTU on socket = " + mtu);

        // I don't know how to set the MTU in java, so skip this set for now

        // 20 bytes = normal IPv4 packet header, 8 bytes = max UDP packet header
        maxUdpPayload = (mtu - 20 - 8 - HEADER_BYTES);




        // Debug printout to check the consistency of the h5. hipoPointer = 61345645 (EV4a)
//            int hipoPointer = outBuffer.getInt();
//            int hipoSize = outBuffer.getInt();
//            System.out.println("DDD:Reader hipoPoint = "
//                    + String.format("%x", hipoPointer)
//                    + " HipoSize = " + hipoSize);

//            ByteBuffer payload = ByteBuffer.allocate(evtLength + 8)
//                    .putInt(eventNumber) //tick
//                    .putInt(evtLength) // length
//                    .put(outBuffer);


        }
        catch (Exception e) {
            e.printStackTrace();
        }


    }


    /**
     * <p>
     * Returns the maximum transmission unit of the network interface used by
     * {@code socket}, or a reasonable default if there's an error retrieving
     * it from the socket.</p>
     *
     * The returned value should only be used as an optimization; such as to
     * size buffers efficiently.
     *
     * @param socket socket to get MTU from.
     * @return MTU value.
     */
    static int getMTU(DatagramSocket socket) {
        try {
            NetworkInterface networkInterface = NetworkInterface.getByInetAddress(
                    socket.getLocalAddress());
            if (networkInterface != null) {
                int mtu = networkInterface.getMTU();
                mtu = mtu > 9000 ? 9000 : mtu;
                return mtu;
            }
            return DEFAULT_MTU;
        } catch (SocketException exception) {
            return DEFAULT_MTU;
        }
    }


    /**
     * Turn short into byte array.
     * Avoids creation of new byte array with each call.
     *
     * @param data short to convert
     * @param byteOrder byte order of returned bytes (big endian if null)
     * @param dest array in which to store returned bytes
     * @param off offset into dest array where returned bytes are placed
     */
    public static void toBytes(short data, ByteOrder byteOrder, byte[] dest, int off) {
        if (byteOrder == null || byteOrder == ByteOrder.BIG_ENDIAN) {
            dest[off  ] = (byte)(data >>> 8);
            dest[off+1] = (byte)(data      );
        }
        else {
            dest[off  ] = (byte)(data      );
            dest[off+1] = (byte)(data >>> 8);
        }
    }

    /**
     * Turn int into byte array.
     * Avoids creation of new byte array with each call.
     *
     * @param data int to convert
     * @param byteOrder byte order of returned bytes (big endian if null)
     * @param dest array in which to store returned bytes
     * @param off offset into dest array where returned bytes are placed
     */
    public static void toBytes(int data, ByteOrder byteOrder, byte[] dest, int off) {
        if (byteOrder == null || byteOrder == ByteOrder.BIG_ENDIAN) {
            dest[off  ] = (byte)(data >> 24);
            dest[off+1] = (byte)(data >> 16);
            dest[off+2] = (byte)(data >>  8);
            dest[off+3] = (byte)(data      );
        }
        else {
            dest[off  ] = (byte)(data      );
            dest[off+1] = (byte)(data >>  8);
            dest[off+2] = (byte)(data >> 16);
            dest[off+3] = (byte)(data >> 24);
        }
    }

    /**
     * Turn long into byte array.
     * Avoids creation of new byte array with each call.
     *
     * @param data long to convert
     * @param byteOrder byte order of returned bytes (big endian if null)
     * @param dest array in which to store returned bytes
     * @param off offset into dest array where returned bytes are placed
     */
    public static void toBytes(long data, ByteOrder byteOrder, byte[] dest, int off) {
        if (byteOrder == null || byteOrder == ByteOrder.BIG_ENDIAN) {
            dest[off  ] = (byte)(data >> 56);
            dest[off+1] = (byte)(data >> 48);
            dest[off+2] = (byte)(data >> 40);
            dest[off+3] = (byte)(data >> 32);
            dest[off+4] = (byte)(data >> 24);
            dest[off+5] = (byte)(data >> 16);
            dest[off+6] = (byte)(data >>  8);
            dest[off+7] = (byte)(data      );
        }
        else {
            dest[off  ] = (byte)(data      );
            dest[off+1] = (byte)(data >>  8);
            dest[off+2] = (byte)(data >> 16);
            dest[off+3] = (byte)(data >> 24);
            dest[off+4] = (byte)(data >> 32);
            dest[off+5] = (byte)(data >> 40);
            dest[off+6] = (byte)(data >> 48);
            dest[off+7] = (byte)(data >> 56);
        }
    }


    /**
     * <p>
     * Write the reassembly header, at the start of the given byte array,
     * in the format used in ERSAP project.
     * The first 16 bits go as ordered. The dataId is put in network byte order.
     * The offset and tick are also put into network byte order.</p>
     * </p>
     * <pre>
     *  protocol 'Version:4, Rsvd:10, First:1, Last:1, Data-ID:16, Offset:32'
     *
     *  0                   1                   2                   3
     *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *  |Version|        Rsvd       |F|L|            Data-ID            |
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *  |                  UDP Packet Offset                            |
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *  |                                                               |
     *  +                              Tick                             +
     *  |                                                               |
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     * </pre>
     *
     * @param buffer        byte array in which to write.
     * @param offset        index in buffer to start writing.
     * @param version       version of meta data.
     * @param first         is this the first packet of a record/buffer being sent?
     * @param last          is this the last packet of a record/buffer being sent?
     * @param dataId        data source id.
     * @param packetOffset  packet sequence.
     * @param tick          tick value.
     * @throws EmuException if offset &lt; 0 or buffer overflow.
     */
    static void writeErsapReHeader(byte[] buffer, int offset,
                                   int version, boolean first, boolean last,
                                   short dataId, int packetOffset, long tick)
            throws Exception {

        if (offset < 0 || (offset + 16 > buffer.length)) {
            throw new Exception("offset arg < 0 or buf too small");
        }

        buffer[offset] = (byte) (version << 4);
        int fst = first ? 1 : 0;
        int lst =  last ? 1 : 0;
        buffer[offset + 1] = (byte) ((fst << 1) + lst);

        toBytes(dataId, ByteOrder.BIG_ENDIAN, buffer, offset + 2);
        toBytes(packetOffset, ByteOrder.BIG_ENDIAN, buffer, offset + 4);
        toBytes(tick, ByteOrder.BIG_ENDIAN, buffer, offset + 8);
    }


    /**
     * Set the Load Balancer header data.
     * The first four bytes go as ordered.
     * The entropy goes as a single, network byte ordered, 16-bit int.
     * The tick goes as a single, network byte ordered, 64-bit int.
     *
     * <pre>
     *  protocol 'L:8,B:8,Version:8,Protocol:8,Reserved:16,Entropy:16,Tick:64'
     *
     *  0                   1                   2                   3
     *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *  |       L       |       B       |    Version    |    Protocol   |
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *  3               4                   5                   6
     *  2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *  |              Rsvd             |            Entropy            |
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *  6                                               12
     *  4 5       ...           ...         ...         0 1 2 3 4 5 6 7
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *  |                                                               |
     *  +                              Tick                             +
     *  |                                                               |
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     * </pre>
     *
     * @param buffer   buffer in which to write the header.
     * @param off      index in buffer to start writing.
     * @param tick     unsigned 64 bit tick number used to tell the load balancer
     *                 which backend host to direct the packet to.
     * @param version  version of load balancer metadata.
     * @param protocol protocol this software uses.
     * @param entropy  entropy field used to determine destination port.
     * @return bytes written.
     * @throws EmuException if offset &lt; 0 or buffer overflow.
     */
    static int writeLbHeader(ByteBuffer buffer, int off, long tick, int version, int protocol, int entropy)
            throws Exception {

        if (off < 0 || (off + 16 > buffer.limit())) {
            throw new Exception("offset arg < 0 or buf too small");
        }

        buffer.put(off, (byte)('L'));
        buffer.put(off+1, (byte)('B'));
        buffer.put(off+2, (byte)version);
        buffer.put(off+3, (byte)protocol);
        buffer.putShort(off+6, (short)entropy);
        buffer.putLong(off+8, tick);
        return 16;
    }


    /**
     * Set the Load Balancer header data.
     * The first four bytes go as ordered.
     * The entropy goes as a single, network byte ordered, 16-bit int.
     * The tick goes as a single, network byte ordered, 64-bit int.
     *
     * <pre>
     *  protocol 'L:8,B:8,Version:8,Protocol:8,Reserved:16,Entropy:16,Tick:64'
     *
     *  0                   1                   2                   3
     *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *  |       L       |       B       |    Version    |    Protocol   |
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *  3               4                   5                   6
     *  2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *  |              Rsvd             |            Entropy            |
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *  6                                               12
     *  4 5       ...           ...         ...         0 1 2 3 4 5 6 7
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *  |                                                               |
     *  +                              Tick                             +
     *  |                                                               |
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     * </pre>
     *
     * @param buffer   buffer in which to write the header.
     * @param off      index in buffer to start writing.
     * @param tick     unsigned 64 bit tick number used to tell the load balancer
     *                 which backend host to direct the packet to.
     * @param version  version of load balancer metadata.
     * @param protocol protocol this software uses.
     * @param entropy  entropy field used to determine destination port.
     * @return bytes written.
     * @throws EmuException if offset &lt; 0 or buffer overflow.
     */
    static int writeLbHeader(byte[] buffer, int off, long tick, int version, int protocol, int entropy)
            throws Exception {

        if (off < 0 || (off + 16 > buffer.length)) {
            throw new Exception("offset arg < 0 or buf too small");
        }

        buffer[off]   = (byte) 'L';
        buffer[off+1] = (byte) 'B';
        buffer[off+2] = (byte) version;
        buffer[off+3] = (byte) protocol;

        toBytes((short)entropy, ByteOrder.BIG_ENDIAN, buffer, off+6);
        toBytes(tick, ByteOrder.BIG_ENDIAN, buffer, off+8);

        return 16;
    }


    /** <p>
     * Send a buffer to a given destination by breaking it up into smaller
     * packets and sending these by UDP.
     * The receiver is responsible for reassembling these packets back into the original data.</p>
     *
     * Optimize by minimizing copying of data and calling "send" on a connected socket.
     * The very first packet is sent in buffer of copied data.
     * However, subsequently it writes the new header into the
     * dataBuffer just before the data to be sent, and then sends.
     * <b>Be warned that the original buffer will be changed after calling this routine!
     * This should not be a big deal as emu output channels send out each event only on
     * ONE channel by round-robin. The ER is an exception but only allows file and ET
     * output channels. So things should be fine.</b>
     *
     * @param dataBuffer     data to be sent.
     * @param readFromIndex  index into dataBuffer to start reading.
     * @param dataLen        number of bytes to be sent.
     * @param packetStorage  array in which to build a packet to send.
     *                       It's pssed in as a parameter to avoid object creation and its
     *                       attendant strain on the garbage collector with each call.
     * @param maxUdpPayload  maximum number of bytes to place into one UDP packet.
     *
     * @param clientSocket   UDP sending socket.
     * @param udpPacket      UDP sending packet.
     *
     * @param tick           value used by load balancer (LB) in directing packets to final host.
     * @param entropy        entropy used by LB header in directing packets to a specific port
     *                       (but currently unused).
     * @param lbProtocol     protocol in LB header.
     * @param lbVersion      verion of LB header.
     *
     * @param recordId       record id in reassembly (RE) header.
     * @param dataId         data id in RE header.
     * @param reVersion      version of RE header.
     *
     * @param delay          delay in millisec between each packet being sent.
     * @param debug          turn debug printout on & off.
     * @param packetsSent    Array with one element.
     *                       Used to return the number of packets sent over network
     *                              (valid even if error returned).
     * @throws IOException if error sending packets
     */
    static void sendPacketizedBufferFast(byte[] dataBuffer, int readFromIndex, int dataLen,
                                         byte[] packetStorage, int maxUdpPayload,
                                         DatagramSocket clientSocket, DatagramPacket udpPacket,
                                         long tick, int entropy, int lbProtocol, int lbVersion,
                                         int recordId, int dataId, int reVersion,
                                         int delay, boolean debug,
                                         int[] packetsSent)
            throws IOException {

        int bytesToWrite, sentPackets = 0;

        // How many total packets are we sending? Round up.
        int totalPackets = (dataLen + maxUdpPayload - 1)/maxUdpPayload;

        // The very first packet goes in here
        //byte[] packetStorage = new byte[maxUdpPayload + HEADER_BYTES];
        // Index into packetStorage to write
        int writeToIndex = 0;

        // If this packet is the very first packet sent for this data buffer
        boolean veryFirstPacket = true;
        // If this packet is the very last packet sent for this data buffer
        boolean veryLastPacket  = false;

        int packetCounter = 0;
        // Use this flag to allow transmission of a single zero-length buffer
        boolean firstLoop = true;

        while (firstLoop || dataLen > 0) {

            // The number of regular data bytes to write into this packet
            bytesToWrite = dataLen > maxUdpPayload ? maxUdpPayload : dataLen;

            // Is this the very last packet for buffer?
            if (bytesToWrite == dataLen) {
                veryLastPacket = true;
            }

            if (debug) System.out.println("Send " + bytesToWrite +
                    " bytes, very first = " + veryFirstPacket +
                    ", very last = " + veryLastPacket +
                    ", total packets = " + totalPackets +
                    ", packet counter = " + packetCounter +
                    ", writeToIndex = " + writeToIndex);

            // Write LB meta data into buffer
            try {
                // Write LB meta data into byte array
//logger.info("    DataChannel UDP stream: LB header: tick = " + tick + ", entropy = " + entropy);
                writeLbHeader(packetStorage, writeToIndex, tick, lbVersion, lbProtocol, entropy);

                // Write RE meta data into byte array
                writeErsapReHeader(packetStorage, writeToIndex + LB_HEADER_BYTES,
                        reVersion, veryFirstPacket, veryLastPacket, (short)dataId,
                        packetCounter++, tick);
            }
            catch (Exception e) {/* never happen */}

            if (firstLoop) {
                // Copy data for very first packet only
                System.arraycopy(dataBuffer, readFromIndex,
                        packetStorage, writeToIndex + HEADER_BYTES,
                        bytesToWrite);
            }

            // "UNIX Network Programming" points out that a connect call made on a UDP client side socket
            // figures out and stores all the state about the destination socket address in advance
            // (masking, selecting interface, etc.), saving the cost of doing so on every send call.
            // This book claims that a connected socket can be up to 3x faster because of this reduced overhead -
            // data can go straight to the NIC driver bypassing most IP stack processing.
            // In our case, the calling function connected the socket.

            // Send message to receiver
            udpPacket.setData(packetStorage, writeToIndex, bytesToWrite + HEADER_BYTES);
            clientSocket.send(udpPacket);

            if (firstLoop) {
                // Switch from external array to writing from dataBuffer for rest of packets
                packetStorage = dataBuffer;
                writeToIndex = -1 * HEADER_BYTES;
            }

            sentPackets++;

            // delay if any
            if (delay > 0) {
                try {
                    Thread.sleep(delay);
                }
                catch (InterruptedException e) {}
            }

            dataLen -= bytesToWrite;
            writeToIndex += bytesToWrite;
            readFromIndex += bytesToWrite;
            veryFirstPacket = false;
            firstLoop = false;

            if (debug) System.out.println("Sent pkt " + (packetCounter - 1) +
                    ", remaining bytes = " + dataLen + "\n");
        }

        packetsSent[0] = sentPackets;
    }


}
