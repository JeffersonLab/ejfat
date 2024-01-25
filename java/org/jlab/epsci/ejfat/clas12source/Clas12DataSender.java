package org.jlab.epsci.ejfat.clas12source;


import java.io.*;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.net.*;
import java.time.Instant;

import j4np.hipo5.data.Event;
import j4np.hipo5.io.HipoReader;

/**
 * <p>
 * @file Read the given HIPO data file and send each event in it
 * to an ejfat router (FPGA-based or simulated) which then passes it
 * to the receiving program - possibly packetBlasteeEtFifoClient.cc .
 * Try /daqfs/java/clas_005038.1231.hipo on the DAQ group disk.
 * </p>
 * <p>
 * This program creates 1 to 16 output UDP sockets and rotates between them when
 * sending each event/buffer. This is to facilitate efficient switch operation.
 * The variation in port numbers gives the switch more "entropy",
 * according to ESNET, since each connection is defined by source & host IP and port #s
 * and the switching algorithm is stateless - always relying on these 4 parameters.
 * This makes 16 possibilities or 4 bits of entropy in which ports must be different
 * but not necessarily sequential.
 * </p>
 * @author timmer
 * Date: Jan 25, 2024
 */
public class Clas12DataSender {

    /** Size of EJFAT load balancing header */
    static final int LB_HEADER_BYTES = 16;
    /** Size of UDP EJFAT reassembly header. */
    static final int RE_HEADER_BYTES = 20;
    /** Size of all headers. */
    static final int HEADER_BYTES = RE_HEADER_BYTES + LB_HEADER_BYTES;
    /** Default MTU to be used if it cannot be found programmatically. */
    static final int DEFAULT_MTU = 1400;
    /** Maximum number of sockets used to send data. */
    static final int MAX_SOCK_COUNT = 16;



    private boolean debug, sync, bufDelay, direct, connect=true;

    private int repeat = Integer.MAX_VALUE;
    private String filename;

    private int delay;  // delay in microsec
    private int delayPrescale = 1;
    private int tickPrescale = 1;

    private int sendBufSize = 25000000; // 50 MB UDP send buffer is default
    private int port = 0x4c42; // 19522, FPGA port default
    private String destHost; // no default

    private int cpPort = 0x4c43; // 19523 CP port default
    private String cpHost; // no default

    private int socketCount = 1;
    private long tick;
    private int entropy;
    private int srcId;

    /** FPGA Load Balancer reassembly protocol. */
    private int lbProtocol = 1;
    /** FPGA Load Balancer reassembly version. LB only works for version = 2. */
    private int lbVersion = 2;
    /** ERSAP reassembly version */
    private final int reVersion = 1;

    private int mtu = DEFAULT_MTU;



    /** Constructor. */
    Clas12DataSender(String[] args) {
        decodeCommandLine(args);
    }


    /**
     * Method to decode the command line used to start this application.
     * @param args command line arguments
     */
    private void decodeCommandLine(String[] args) {

        try {
            // loop over all args
            for (int i = 0; i < args.length; i++) {

                if (args[i].equalsIgnoreCase("-h")) {
                    usage();
                    System.exit(-1);
                }
                else if (args[i].equalsIgnoreCase("-f")) {
                    filename = args[i+1];
                    i++;
                }
                else if (args[i].equalsIgnoreCase("-v")) {
                    debug = true;
                }
                else if (args[i].equalsIgnoreCase("-sync")) {
                    sync = true;
                }
                else if (args[i].equalsIgnoreCase("-direct")) {
                    direct = true;
                }
                else if (args[i].equalsIgnoreCase("-nc")) {
                    connect = false;
                }
                else if (args[i].equalsIgnoreCase("-r")) {
                    try {
                        repeat = Integer.parseInt(args[i+1]);
                        if (repeat < 1) repeat = 1;
                    }
                    catch (NumberFormatException e) {}
                    i++;
                }


                else if (args[i].equalsIgnoreCase("-bufDelay")) {
                    bufDelay = true;
                }
                else if (args[i].equalsIgnoreCase("-d")) {
                    try {
                        delay = Integer.parseInt(args[i+1]);
                        if (delay < 0) delay = 0;
                    }
                    catch (NumberFormatException e) {}
                    i++;
                }
                else if (args[i].equalsIgnoreCase("-dpre")) {
                    try {
                        delayPrescale = Integer.parseInt(args[i+1]);
                        if (delayPrescale < 1) delayPrescale = 1;
                    }
                    catch (NumberFormatException e) {}
                    i++;
                }
                else if (args[i].equalsIgnoreCase("-tpre")) {
                    try {
                        tickPrescale = Integer.parseInt(args[i+1]);
                        if (tickPrescale < 1) tickPrescale = 1;
                    }
                    catch (NumberFormatException e) {}
                    i++;
                }


                else if (args[i].equalsIgnoreCase("-host")) {
                    destHost = args[i+1];
                    i++;
                }
                else if (args[i].equalsIgnoreCase("-p")) {
                    try {
                        port = Integer.parseInt(args[i+1]);
                        if (port > 65535 || port < 1024) {
                            throw new Exception("port number must be < 65536 and > 1023");
                        }
                    }
                    catch (NumberFormatException e) {}
                    i++;
                }


                else if (args[i].equalsIgnoreCase("-cp_host")) {
                    cpHost = args[i+1];
                    i++;
                }
                else if (args[i].equalsIgnoreCase("-cp_port")) {
                    try {
                        cpPort = Integer.parseInt(args[i+1]);
                        if (cpPort > 65535 || cpPort < 1024) {
                            throw new Exception("port number must be < 65536 and > 1023");
                        }
                    }
                    catch (NumberFormatException e) {}
                    i++;
                }
                else if (args[i].equalsIgnoreCase("-sock")) {
                    try {
                        socketCount = Integer.parseInt(args[i+1]);
                        if (socketCount < 1) {
                            socketCount = 1;
                        }
                        else if (socketCount > MAX_SOCK_COUNT) {
                            socketCount = MAX_SOCK_COUNT;
                        }
                    }
                    catch (NumberFormatException e) {}
                    i++;
                }


                else if (args[i].equalsIgnoreCase("-s")) {
                    try {
                        sendBufSize = Integer.parseInt(args[i+1]);
                        if (sendBufSize < 100000) {
                            throw new Exception("keep UDP buf size > 100kB");
                        }
                    }
                    catch (NumberFormatException e) {}
                    i++;
                }
                else if (args[i].equalsIgnoreCase("-mtu")) {
                    try {
                        mtu = Integer.parseInt(args[i+1]);
                        if (mtu > 9000) mtu = 9000;
                        else if (mtu < 1000) mtu = 1000;
                    }
                    catch (NumberFormatException e) {}
                    i++;
                }
                else if (args[i].equalsIgnoreCase("-t")) {
                    try {
                        tick = Long.parseLong(args[i+1]);
                        if (tick < 0) tick = 0;
                    }
                    catch (NumberFormatException e) {}
                    i++;
                }
                else if (args[i].equalsIgnoreCase("-ver")) {
                    try {
                        lbVersion = Integer.parseInt(args[i+1]);
                        if (lbVersion < 1) lbVersion = 2;
                    }
                    catch (NumberFormatException e) {}
                    i++;
                }
                else if (args[i].equalsIgnoreCase("-pro")) {
                    try {
                        lbProtocol = Integer.parseInt(args[i+1]);
                        if (lbProtocol < 1) lbProtocol = 1;
                    }
                    catch (NumberFormatException e) {}
                    i++;
                }
                else if (args[i].equalsIgnoreCase("-e")) {
                    try {
                        entropy = Integer.parseInt(args[i+1]);
                        if (entropy < 0) entropy = 0;
                    }
                    catch (NumberFormatException e) {}
                    i++;
                }
                else if (args[i].equalsIgnoreCase("-id")) {
                    try {
                        srcId = Integer.parseInt(args[i+1]);
                        if (srcId < 0) srcId = 0;
                    }
                    catch (NumberFormatException e) {}
                    i++;
                }


                else {
                    usage();
                    System.exit(-1);
                }
            }

            if (filename == null) {
                System.out.println("\nProvide -f option\n\n");
                usage();
                System.exit(-1);
            }

            if (destHost == null) {
                System.out.println("\nProvide -host option\n\n");
                usage();
                System.exit(-1);
            }

            if (sync && cpHost == null) {
                System.out.println("\nProvide -cp_host option if using -sync\n\n");
                usage();
                System.exit(-1);
            }
        }
        catch (Exception e) {
            System.out.println(e.getMessage());
            usage();
        }
    }

    /** Method to print out correct program command line usage. */
    private static void usage() {

        System.out.println("\nUsage:\n\n" +
                "   java Clas12DataSender\n" +

                "        -f <filename>              name of data file\n" +
                "        [-h] [-v]\n" +
                "        [-r <# repeats>]           # times to read file (default forever)\n" +
                "        [-sync]                    send 1 Hz msg to control plane w/ event #\n" +
                "        [-direct]                  send data directly to back end, bypass LB\n" +
                "        [-nc]                      do NOT connect socket\n\n" +

                "        [-d <delay in microsec>]\n" +
                "        [-bufdelay]                delay between each buffer (default between packets)\n\n" +
                "        [-tpre <tick prescale>     tick increment after each event sent)\n" +
                "        [-dpre <delay prescale>    if -d defined, 1 delay after every prescale pkts/evts)>]" +

                "        [-host <data dest>]        LB host (no default)\n" +
                "        [-p <data dest UDP port>   LB port (default 19522)\n\n" +

                "        [-cp_host <CP IP addr>]    sync msg host (no default)\n" +
                "        [-cp_port <CP port>]       sync msg port (default 19523)\n" +
                "        [-sock <# UDP sockets>]    sockets used to send data, 16 max (default 1)\n\n" +

                "        [-mtu <desired MTU size>]\n" +
                "        [-t <tick>]\n" +
                "        [-ver <LB version>]\n" +
                "        [-id <data src id>]\n" +
                "        [-pro <protocol>]\n" +
                "        [-e <entropy>]\n" +
                "        [-s <UDP send buf size>]  (default 25MB which linux increases to 50MB)");
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

            // Figure where delay is placed, between packets or events/buffers
            int bufferDelay  = 0;
            int packetDelay = 0;

            if (delay > 0) {
                if (bufDelay) {
                    bufferDelay = delay;
                }
                else {
                    packetDelay = delay;
                }
            }

            // Socket for sending sync message to CP
            byte[] syncStore = new byte[28];
            DatagramPacket syncPacket = null;
            DatagramSocket syncSocket = null;

            if (sync) {
System.out.println("    Create sync UDP socket to dest " + cpHost + " port " + cpPort);
                syncSocket = new DatagramSocket();
                syncSocket.setSendBufferSize(sendBufSize);
                InetAddress destAddr = InetAddress.getByName(cpHost);
                if (connect) {
                    syncSocket.connect(destAddr, cpPort);
                    syncPacket = new DatagramPacket(syncStore, 28);
                }
                else {
                    syncPacket = new DatagramPacket(syncStore, 28, destAddr, cpPort);
                }
            }


            // Create (possibly) multiple UDP sockets for efficient switch operation
            DatagramSocket[] clientSockets = new DatagramSocket[socketCount];
            DatagramPacket[] udpPackets    = new DatagramPacket[socketCount];
            byte[][] packetStorage = new byte[socketCount][65535];

            for (int i = 0; i < socketCount; i++) {
System.out.println("    Create data UDP socket #" + i + " to dest " + destHost + " port " + port);
                clientSockets[i] = new DatagramSocket();
                clientSockets[i].setSendBufferSize(sendBufSize);
                InetAddress destAddr = InetAddress.getByName(destHost);
                if (connect) {
                    clientSockets[i].connect(destAddr, port);
                }

                if (connect) {
                    udpPackets[i] = new DatagramPacket(packetStorage[i], 65536);
                }
                else {
                    udpPackets[i] = new DatagramPacket(packetStorage[i], 65536, destAddr, port);
                }
            }


            // Break data into multiple packets of MTU size.
            // Attempt to get MTU progamatically.
            if (mtu == 0) {
                mtu = getMTU(clientSockets[0]);
            }

            // Jumbo frames are 9000 bytes max.
            if (mtu > 9000) {
                mtu = 9000;
            }
            System.out.println("MTU on socket = " + mtu);

            // 20 bytes = normal IPv4 packet header, 8 bytes = max UDP packet header
            // https://stackoverflow.com/questions/42609561/udp-maximum-packet-size
            int maxUdpPayload = (mtu - 20 - 8 - HEADER_BYTES);


            int readFromIndex = 0;
            int delayCounter = 0;
            int portIndex = 0;
            int evtRate;
            int[] packetsSent = new int[2];
            long loops = repeat, bufsSent = 0, totalBytes = 0, totalPackets = 0, totalEvents = 0;

            long deltaT;
            Instant instant = Instant.now();
            long startTimeNanoos = instant.getEpochSecond() * 1000_000_000 + instant.getNano();

            HipoReader reader = new HipoReader();
            reader.open(filename);
            Event event = new Event();

            while (true) {

                if (reader.hasNext()) {
                    reader.nextEvent(event);
                }
                else {
                    reader.rewind();
                    reader.nextEvent(event);
                }

                int evtLength = event.getEventBufferSize();

                ByteBuffer eventBuffer = event.getEventBuffer();
                eventBuffer.rewind();

                byte[] evt = new byte[evtLength];
                eventBuffer.get(evt);


                sendPacketizedBuffer(evt, readFromIndex, evtLength,
                        maxUdpPayload,
                        clientSockets[portIndex], udpPackets[portIndex],
                        tick, entropy, lbProtocol, lbVersion,
                        srcId, reVersion,
                        packetDelay, debug,
                        delayPrescale, delayCounter,
                        direct, packetsSent);

                bufsSent++;
                totalBytes   += evtLength;
                totalPackets += packetsSent[0];
                delayCounter  = packetsSent[1];
                totalEvents++;
                tick += tickPrescale;

                if (sync) {

                    Instant.now();
                    long curTimeNanoos = instant.getEpochSecond() * 1000_000_000 + instant.getNano();
                    deltaT = curTimeNanoos - startTimeNanoos;

                    // if >= 1 sec ...
                    if (deltaT >= 1000_000_000) {
                        // Calculate buf or event rate in Hz
                        evtRate = (int)(bufsSent/(deltaT/1000_000_000));

                        // Send sync message to control plane
                        if (debug) System.out.println("sync: tick " + tick + ", evtRate " + evtRate + "\n\n");

                        setSyncData(syncStore, 0, lbVersion, srcId, tick, evtRate, curTimeNanoos);
                        syncSocket.send(syncPacket);

                        startTimeNanoos = curTimeNanoos;
                        bufsSent = 0;
                    }
                }

                portIndex = (portIndex + 1) % socketCount;

                // delay if any
                if (bufDelay) {
                    if (--delayCounter < 1) {
                        Thread.sleep(bufferDelay);
                        delayCounter = delayPrescale;
                    }
                }

                if (--loops < 1) {
                    System.out.println("\nClas12DataSemder: finished " + repeat + " loops reading & sending buffers from file\n\n");
                    break;
                }
            }
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
                mtu = Math.min(mtu, 9000);
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
     * The offset, length and tick are also put into network byte order.</p>
     * This is the new, version 2, RE header.
     *
     * <pre>
     *  protocol 'Version:4, Rsvd:12, Data-ID:16, Offset:32, Length:32, Tick:64'
     *
     *  0                   1                   2                   3
     *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *  |Version|        Rsvd           |            Data-ID            |
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *  |                         Buffer Offset                         |
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *  |                         Buffer Length                         |
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *  |                                                               |
     *  +                             Tick                              +
     *  |                                                               |
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     * </pre>
     *
     * @param buffer        byte array in which to write.
     * @param offset        index in buffer to start writing.
     * @param version       version of meta data (should be 2).
     * @param dataId        data source id.
     * @param bufferOffset  byte offset into full buffer payload.
     * @param bufferLength  total length in bytes of full buffer payload.
     * @param tick          tick value.
     * @throws Exception    if offset &lt; 0 or buffer overflow.
     */
     static void writeErsapReHeader(byte[] buffer, int offset,
                                      int version, short dataId,
                                      int bufferOffset, int bufferLength, long tick)
            throws Exception {

        if (offset < 0 || (offset + RE_HEADER_BYTES > buffer.length)) {
            throw new Exception("offset arg < 0 or buf too small");
        }

        buffer[offset] = (byte) (version << 4);

        toBytes(dataId, ByteOrder.BIG_ENDIAN, buffer, offset + 2);
        toBytes(bufferOffset, ByteOrder.BIG_ENDIAN, buffer, offset + 4);
        toBytes(bufferLength, ByteOrder.BIG_ENDIAN, buffer, offset + 8);
        toBytes(tick, ByteOrder.BIG_ENDIAN, buffer, offset + 12);
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
     * @throws Exception if offset &lt; 0 or buffer overflow.
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
     * @throws Exception if offset &lt; 0 or buffer overflow.
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


    /**
     * <p>
     * Set the data for a synchronization message sent directly to the load balancer.
     * The first 3 fields go as ordered. The srcId, evtNum, evtRate and time are all
     * put into network byte order.</p>
     *
     * <pre>
     *  protocol 'Version:4, Rsvd:12, Data-ID:16, Offset:32, Length:32, Tick:64'
     *
     *    0                   1                   2                   3
     *    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
     *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *    |       L       |       C       |    Version    |      Rsvd     |
     *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *    |                           EventSrcId                          |
     *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *    |                                                               |
     *    +                          EventNumber                          +
     *    |                                                               |
     *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *    |                         AvgEventRateHz                        |
     *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *    |                                                               |
     *    +                          UnixTimeNano                         +
     *    |                                                               |
     *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     * </pre>
     *
     * @param buffer   buffer in which to write the data.
     * @param version  version of this software.
     * @param srcId    id number of this data source.
     * @param evtNum   unsigned 64 bit event number used to tell the load balancer
     *                 which backend host to direct the packet to. This message
     *                 is telling the load balancer that this application has
     *                 already sent this, latest, event.
     * @param evtRate  in Hz, the rate this application is sending events
     *                 to the load balancer (0 if unknown).
     * @param nanos    at what unix time in nanoseconds was this message sent (0 if unknown).
     */
    static void setSyncData(byte[] buffer, int off, int version, int srcId,
                            long evtNum, int evtRate, long nanos) throws Exception {

        if (off < 0 || (off + 28 > buffer.length)) {
            throw new Exception("offset arg < 0 or buf too small");
        }

        buffer[off]   = (byte) 'L';
        buffer[off+1] = (byte) 'C';
        buffer[off+2] = (byte) version;

        toBytes(srcId,   ByteOrder.BIG_ENDIAN, buffer, off+4);
        toBytes(evtNum,  ByteOrder.BIG_ENDIAN, buffer, off+8);
        toBytes(evtRate, ByteOrder.BIG_ENDIAN, buffer, off+16);
        toBytes(nanos,   ByteOrder.BIG_ENDIAN, buffer, off+20);
    }


    /**
     * <p>
     * This routine uses the latest, version 2, RE header.
     * Send the given buffer to a given destination by breaking it up into smaller
     * packets and sending these by UDP.
     * The receiver is responsible for reassembling these packets back into the original data.
     * </p>
     *
     * All data (header and actual data from dataBuffer arg) are copied into a separate
     * buffer (packetStorage) and sent. The original data is unchanged.
     *
     * @param dataBuffer     data to be sent.
     * @param readFromIndex  index into dataBuffer to start reading.
     * @param dataLen        number of bytes to be sent.
     * @param maxUdpPayload  maximum number of bytes to place into one UDP packet.
     *
     * @param clientSocket   UDP sending socket.
     * @param udpPacket      UDP sending packet (contains byte array to hold data)
     *
     * @param tick           value used by load balancer (LB) in directing packets to final host.
     * @param entropy        entropy used by LB header in directing packets to a specific port
     *                       (but currently unused).
     * @param protocol       protocol in LB header.
     * @param lbVersion      version in LB header.
     *
     * @param dataId         data id in RE (reassembly) header.
     * @param reVersion      version of RE header.
     *
     * @param delay          delay in microsec between each packet being sent.
     * @param debug          turn debug printout on & off.
     *
     * @param delayPrescale  prescale for delay (i.e. only delay every Nth time).
     * @param delayCounter   value-result parameter tracking when delay was last run.
     *
     * @param direct         don't include LB header since packets are going directly to receiver.
     * @param packetsSent    first element filled with number of packets sent over network (valid even if error returned),
     *                       second element is last value of delayCounter for possible use in next
     *                       call to this method.
     * @throws IOException   if I/O Error when sending packet.
     */
    static void sendPacketizedBuffer(byte[] dataBuffer, int readFromIndex, int dataLen,
                                     int maxUdpPayload,
                                     DatagramSocket clientSocket, DatagramPacket udpPacket,
                                     long tick, int entropy, int protocol, int lbVersion,
                                     int dataId, int reVersion,
                                     int delay, boolean debug,
                                     int delayPrescale, int delayCounter,
                                     boolean direct,
                                     int[] packetsSent) throws IOException {

        int err;
        int bytesToWrite,  sentPackets=0;
        // Offset for the packet currently being sent (into full buffer)
        int bufOffset = 0;
        int remainingBytes = dataLen;

        int packetCounter = 0;
        // Use this flag to allow transmission of a single zero-length buffer
        boolean firstLoop = true;

        byte[] packetStorage = udpPacket.getData();
        if (packetStorage.length < maxUdpPayload) {
            packetStorage = new byte[maxUdpPayload];
        }

        int lbHeaderSize   = LB_HEADER_BYTES;
        int allHeadersSize = HEADER_BYTES;
        // If we bypass LB, don't include that header
        if (direct) {
            lbHeaderSize   = 0;
            allHeadersSize = RE_HEADER_BYTES;
        }

        while (firstLoop || remainingBytes > 0) {

            firstLoop = false;

            // The number of regular data bytes to write into this packet
            bytesToWrite = Math.min(remainingBytes, maxUdpPayload);

            if (debug) System.out.println("Send " + bytesToWrite +
                    ", packet counter = " + packetCounter);

            try {
                if (!direct) {
                    // Write LB meta data into buffer
                    writeLbHeader(packetStorage, 0, tick, lbVersion, protocol, entropy);
                }

                // Write RE meta data into buffer
                writeErsapReHeader(packetStorage, lbHeaderSize,
                        reVersion, (short)dataId, bufOffset, dataLen, tick);
            }
            catch (Exception e) {/* never happen */}

            // Copy data
            System.arraycopy(dataBuffer, readFromIndex,
                             packetStorage, allHeadersSize,
                             bytesToWrite);

            // Send message to receiver
            udpPacket.setLength(bytesToWrite + allHeadersSize);
            clientSocket.send(udpPacket);

            sentPackets++;

            // delay if any
            if (delay > 0) {
                try {
                    if (--delayCounter < 1) {
                        Thread.sleep(delay);
                        delayCounter = delayPrescale;
                    }
                }
                catch (InterruptedException e) {}
            }

            bufOffset      += bytesToWrite;
            remainingBytes -= bytesToWrite;
            readFromIndex  += bytesToWrite;

            if (debug) System.out.println("Sent pkt " + (packetCounter++) +
                    ", remaining bytes = " + dataLen + "\n");

        }

        packetsSent[0] = sentPackets;
        packetsSent[1] = delayCounter;
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
     * This is for version 2 of the RE header.
     *
     * @param dataBuffer     data to be sent.
     * @param readFromIndex  index into dataBuffer to start reading.
     * @param dataLen        number of bytes to be sent.
     * @param maxUdpPayload  maximum number of bytes to place into one UDP packet.
     *
     * @param clientSocket   UDP sending socket.
     * @param udpPacket      UDP sending packet.
     *
     * @param tick           value used by load balancer (LB) in directing packets to final host.
     * @param entropy        entropy used by LB header in directing packets to a specific port
     *                       (but currently unused).
     * @param lbProtocol     protocol in LB header.
     * @param lbVersion      version of LB header.
     *
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
                                         int maxUdpPayload,
                                         DatagramSocket clientSocket, DatagramPacket udpPacket,
                                         long tick, int entropy, int lbProtocol, int lbVersion,
                                         int dataId, int reVersion,
                                         int delay, boolean debug,
                                         int[] packetsSent)
            throws IOException {

        int bytesToWrite, sentPackets = 0;
        // Offset for the packet currently being sent (into full buffer)
        int bufOffset = 0;
        int fullLen = dataLen;

        // How many total packets are we sending? Round up.
        int totalPackets = (dataLen + maxUdpPayload - 1)/maxUdpPayload;
        byte[] packetStorage = udpPacket.getData();
        if (packetStorage.length < maxUdpPayload) {
            packetStorage = new byte[maxUdpPayload];
        }

        // Index into packetStorage to write
        int writeToIndex = 0;

        int packetCounter = 0;
        // Use this flag to allow transmission of a single zero-length buffer
        boolean firstLoop = true;

        while (firstLoop || dataLen > 0) {

            // The number of regular data bytes to write into this packet
            bytesToWrite = dataLen > maxUdpPayload ? maxUdpPayload : dataLen;

            if (debug) System.out.println("Send " + bytesToWrite +
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
                                   reVersion, (short)dataId, bufOffset, fullLen, tick);
            }
            catch (Exception e) {/* never happen */}

            if (firstLoop) {
                // Copy data for very first packet only since we need to write header BEFORE data,
                // but most likely cannot do so directly in the data buffer.
                System.arraycopy(dataBuffer, readFromIndex,
                        packetStorage, writeToIndex + HEADER_BYTES,
                        bytesToWrite);
            }

            // Send message to receiver
            udpPacket.setData(packetStorage, writeToIndex, bytesToWrite + HEADER_BYTES);
            clientSocket.send(udpPacket);

            if (firstLoop) {
                // Switch from external array to writing from dataBuffer for rest of packets.
                // Now we have room to write header into dataBuffer BEFORE the data we're sending.
                // Warning, this messes up the data buffer!
                packetStorage = dataBuffer;
                // We want to start writing 1 header len before the 2nd chunk of data.
                // After "writeToIndex" below is added to bytesToWrite a little further down,
                // this index will be in the correct place. The header will be written there,
                // then the header + data will be sent.
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

            bufOffset      += bytesToWrite;
            dataLen        -= bytesToWrite;
            writeToIndex   += bytesToWrite;
            readFromIndex  += bytesToWrite;
            firstLoop       = false;

            if (debug) System.out.println("Sent pkt " + (packetCounter++) +
                    ", remaining bytes = " + dataLen + "\n");
        }

        packetsSent[0] = sentPackets;
    }



}
