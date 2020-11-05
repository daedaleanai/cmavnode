/* CMAVNode
 * Monash UAS
 *
 * SERIAL CLASS
 * This class extends 'link' and overrides it methods to handle uart communications
 */

#include "serial.h"

serial::serial(const std::string& port,
               const std::string& baudrate,
               bool flowcontrol,
               link_info info_,
               queue<std::pair<mlink*, mavlink_message_t>>* qMavIn):
    io_service_(), port_(io_service_), mlink(info_, qMavIn)
{



    try
    {
        //open the port with connection string
        port_.open(port);


        //configure the port
        port_.set_option(boost::asio::serial_port_base::baud_rate((unsigned int)std::stoi(baudrate)));

        if(flowcontrol)
        {
            port_.set_option(boost::asio::serial_port_base::flow_control(
                                 boost::asio::serial_port_base::flow_control::hardware));
        }
        else
        {
            port_.set_option(boost::asio::serial_port_base::flow_control(
                                 boost::asio::serial_port_base::flow_control::none));
        }


        // Setup 8N1
        port_.set_option(boost::asio::serial_port_base::character_size(8));

        port_.set_option(boost::asio::serial_port_base::parity(
                             boost::asio::serial_port_base::parity::none));

        port_.set_option(boost::asio::serial_port_base::stop_bits(
                             boost::asio::serial_port_base::stop_bits::one));

    }
    catch (boost::system::system_error &error)
    {
        std::cerr << "Error opening Serial Port: " << port << " " << error.what() << std::endl;
        std::cerr << "Link: " << info.link_name << " failed to initialise and is dead" << std::endl;
        qMavOut.shut_down();
    }

    //Start the read and write threads
    write_thread = boost::thread(&serial::runWriteThread, this);

    //Start the receive
    port_.async_read_some(
        boost::asio::buffer(data_in_, MAV_INCOMING_BUFFER_LENGTH),
        boost::bind(&serial::handleReceiveFrom, this,
                    boost::asio::placeholders::error,
                    boost::asio::placeholders::bytes_transferred));

    read_thread = boost::thread(&serial::runReadThread, this);
}

serial::~serial()
{
    //Force run() to return then join thread
    io_service_.stop();
    read_thread.join();

    //force write thread to return then join thread
    qMavOut.shut_down();
    write_thread.join();

    //Debind
    port_.close();
}

void serial::send(uint8_t *buf, std::size_t buf_size)
{
    port_.write_some(
        boost::asio::buffer(buf, buf_size));
}

void serial::processAndSend(mavlink_message_t *msgToConvert)
{
    //pack into buf and get size_t
    uint8_t tmplen = mavlink_msg_to_send_buffer(data_out_, msgToConvert);
    //ERROR HANDLING?

    bool should_drop = shouldDropPacket();
    //send on serial
    if(!should_drop)
        send(data_out_, tmplen);
}

//Async post send callback
void serial::handleSendTo(const boost::system::error_code& error,
                          size_t bytes_recvd)
{
    if (!error && bytes_recvd > 0)
    {
        //Everything was ok
    }
    else
    {
        //There was an error

        if(errorcount++ > SERIAL_PORT_MAX_ERROR_BEFORE_KILL)
        {
            is_kill = true;
            std::cout << "Link " << info.link_name << " is dead" << std::endl;
        }
    }
}

//Async callback receiver
void serial::handleReceiveFrom(const boost::system::error_code& error,
                               size_t bytes_recvd)
{
    if (!error && bytes_recvd > 0)
    {
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
        port_.async_read_some(
            boost::asio::buffer(data_in_, MAV_INCOMING_BUFFER_LENGTH),
            boost::bind(&serial::handleReceiveFrom, this,
                        boost::asio::placeholders::error,
                        boost::asio::placeholders::bytes_transferred));
    }
    else if(bytes_recvd == 0)
    {
        //Sleep a little bit to keep the cpu cool
        boost::this_thread::sleep(boost::posix_time::milliseconds(SERIAL_PORT_SLEEP_ON_NOTHING_RECEIVED));
        port_.async_read_some(
            boost::asio::buffer(data_in_, MAV_INCOMING_BUFFER_LENGTH),
            boost::bind(&serial::handleReceiveFrom, this,
                        boost::asio::placeholders::error,
                        boost::asio::placeholders::bytes_transferred));
    }
    else
    {
        //we have an error
        //need to look into what is causing these but for now just pretend it didn't happen
        boost::this_thread::sleep(boost::posix_time::milliseconds(SERIAL_PORT_SLEEP_ON_NOTHING_RECEIVED));
        port_.async_read_some(
            boost::asio::buffer(data_in_, MAV_INCOMING_BUFFER_LENGTH),
            boost::bind(&serial::handleReceiveFrom, this,
                        boost::asio::placeholders::error,
                        boost::asio::placeholders::bytes_transferred));
    }
}

void serial::runReadThread()
{
    //gets run in thread
    //Because io_service.run() will block while socket is open
    io_service_.run();
}

void serial::runWriteThread()
{
    //block so we dont send before the port is initialized
    boost::this_thread::sleep(boost::posix_time::milliseconds(50));
    //busy wait on the spsc_queue
    mavlink_message_t tmpMsg;

    //thread loop
    while(qMavOut.pop(&tmpMsg))
    {
        out_counter.decrement();
        processAndSend(&tmpMsg);
    }
}
