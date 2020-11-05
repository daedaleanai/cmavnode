/* CMAVNode
 * Monash UAS
 *
 * SOCKET CLASS
 * This class extends 'link' and overrides it methods to handle socket communications
 */

#include "asyncsocket.h"

// Fully defined constructor
asyncsocket::asyncsocket(
    const std::string& host,
    const std::string& hostport,
    const std::string& listenport,
    link_info info_) : io_service_(), mlink(info_),
    socket_(io_service_, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), std::stoi(listenport)))
{

    boost::asio::ip::udp::resolver resolver(io_service_);
    boost::asio::ip::udp::resolver::query query(boost::asio::ip::udp::v4(), host, hostport, boost::asio::ip::resolver_query_base::flags(0));
    boost::asio::ip::udp::resolver::iterator iter = resolver.resolve(query);
    endpoint_ = *iter;



    //Start the read and write threads
    write_thread = boost::thread(&asyncsocket::runWriteThread, this);

    //Start the receive
    receive();

    read_thread = boost::thread(&asyncsocket::runReadThread, this);
}

// Client constructor
asyncsocket::asyncsocket(
    const std::string& host,
    const std::string& hostport,
    link_info info_) : io_service_(), mlink(info_),
    socket_(io_service_, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), 0))
{

    boost::asio::ip::udp::resolver resolver(io_service_);
    boost::asio::ip::udp::resolver::query query(boost::asio::ip::udp::v4(), host, hostport);
    boost::asio::ip::udp::resolver::iterator iter = resolver.resolve(query);
    endpoint_ = *iter;


    //Start the read and write threads
    write_thread = boost::thread(&asyncsocket::runWriteThread, this);

    //Start the receive
    receive();

    read_thread = boost::thread(&asyncsocket::runReadThread, this);
}

// Server constructor
asyncsocket::asyncsocket(
    const std::string& listenport,
    link_info info_) : io_service_(), mlink(info_),
    socket_(io_service_, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), std::stoi(listenport))),
    isServer(true)
{
    //Start the read and write threads
    write_thread = boost::thread(&asyncsocket::runWriteThread, this);

    //Start the receive
    receive();

    read_thread = boost::thread(&asyncsocket::runReadThread, this);
}

// Broadcast constructor
asyncsocket::asyncsocket(bool bcastlock,
                         const std::string& bindaddress,
                         const std::string& bcastaddress,
                         const std::string& bcastport,
                         link_info info_) : io_service_(), mlink(info_),
    socket_(io_service_, boost::asio::ip::udp::endpoint(boost::asio::ip::address::from_string(bindaddress), 0))
{
    socket_.set_option(boost::asio::ip::udp::socket::reuse_address(true));
    socket_.set_option(boost::asio::socket_base::broadcast(true));

    boost::asio::ip::udp::endpoint senderEndpoint(boost::asio::ip::address_v4::from_string(bcastaddress), std::stoi(bcastport));
    endpoint_ = senderEndpoint;

    //Start the read and write threads
    write_thread = boost::thread(&asyncsocket::runWriteThread, this);

    endpointlock = bcastlock;
    //Start the receive
    receive();

    read_thread = boost::thread(&asyncsocket::runReadThread, this);
}

asyncsocket::~asyncsocket()
{
    //Force run() to return then join thread
    io_service_.stop();
    read_thread.join();

    //force write thread to return then join thread
    exitFlag = true;
    write_thread.join();

    //Debind
    socket_.close();
}

void asyncsocket::send(uint8_t *buf, std::size_t buf_size)
{
    if (isServer) {
        const std::chrono::time_point<std::chrono::steady_clock> now = std::chrono::steady_clock::now();
        for (std::list<Client>::iterator iter = clients_.begin(); iter != clients_.end(); ) {
            if (now - iter->lastSignOfLife > std::chrono::seconds(30)) {
                iter = clients_.erase(iter);
            } else {
                socket_.async_send_to(
                    boost::asio::buffer(buf, buf_size), iter->endpoint,
                    boost::bind(&asyncsocket::handleSendTo, this,
                                boost::asio::placeholders::error,
                                boost::asio::placeholders::bytes_transferred));

                ++iter;
            }
        }
    } else {
        socket_.async_send_to(
            boost::asio::buffer(buf, buf_size), endpoint_,
            boost::bind(&asyncsocket::handleSendTo, this,
                        boost::asio::placeholders::error,
                        boost::asio::placeholders::bytes_transferred));
    }
}

void asyncsocket::receive()
{
    // async_receive_from will override endpoint_ so if we want to receive from multiple clients use async_receive
    if(!endpointlock)
    {
        //this one only gets used for broadcast when we want to support multiple clients
        socket_.async_receive(
            boost::asio::buffer(data_in_, MAV_INCOMING_BUFFER_LENGTH),
            boost::bind(&asyncsocket::handleReceiveFrom, this,
                        boost::asio::placeholders::error,
                        boost::asio::placeholders::bytes_transferred));
    }
    else
    {
        socket_.async_receive_from(
            boost::asio::buffer(data_in_, MAV_INCOMING_BUFFER_LENGTH), endpoint_,
            boost::bind(&asyncsocket::handleReceiveFrom, this,
                        boost::asio::placeholders::error,
                        boost::asio::placeholders::bytes_transferred));
    }
}

void asyncsocket::processAndSend(mavlink_message_t *msgToConvert)
{
    //pack into buf and get size_t
    uint8_t tmplen = mavlink_msg_to_send_buffer(data_out_, msgToConvert);
    //ERROR HANDLING?

    bool should_drop = shouldDropPacket();
    //send on socket
    if(!should_drop)
        send(data_out_, tmplen);
}



//Async callback receiver
void asyncsocket::handleReceiveFrom(const boost::system::error_code& error,
                                    size_t bytes_recvd)
{
    if (!error && bytes_recvd > 0)
    {
        if (isServer) {
            const std::list<Client>::iterator iter = std::find_if(clients_.begin(), clients_.end(), [this](const Client& c) { return c.endpoint == endpoint_; });
            if (iter == clients_.end()) {
                clients_.push_back(Client{endpoint_, std::chrono::steady_clock::now()});
            } else {
                iter->lastSignOfLife = std::chrono::steady_clock::now();
            }
        }

        //message received
        //do something
        mavlink_message_t msg;
        mavlink_status_t status;

        for (size_t i = 0; i < bytes_recvd; i++)
        {
            if (mav_parse_char(data_in_[i], &msg, &status))
            {
                onMessageRecv(&msg);
            }
        }

        //And start reading again
        receive();
    }
    else
    {
        //nothing received or there was an error
        throw Exception("UDPClient: Error in handle_receive_from");
    }
}

//Async post send callback
void asyncsocket::handleSendTo(const boost::system::error_code& error,
                               size_t bytes_recvd)
{
    if (!error && bytes_recvd > 0)
    {
        //Everything was ok
    }
    else
    {
        //There was an error
    }
}

void asyncsocket::runReadThread()
{
    //gets run in thread
    //Because io_service.run() will block while socket is open
    io_service_.run();
}

void asyncsocket::runWriteThread()
{
    //block so we dont send before the socket is initialized
    boost::this_thread::sleep(boost::posix_time::milliseconds(50));
    //busy wait on the spsc_queueo
    mavlink_message_t tmpMsg;

    // Thread loop
    while (!exitFlag)
    {
        while (qMavOut.pop(tmpMsg))
        {
            out_counter.decrement();
            processAndSend(&tmpMsg);
        }
        boost::this_thread::sleep(boost::posix_time::milliseconds(OUT_QUEUE_EMPTY_SLEEP));
    }
}
