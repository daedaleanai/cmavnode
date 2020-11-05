/* CMAVNode
 * Monash UAS
 *
 * SERIAL CLASS
 * This class extends 'link' and overrides it methods to handle uart communications
 */
#ifndef SERIAL_H
#define SERIAL_H

#include <string>
#include <boost/asio.hpp>

#include "mlink.h"

#define SERIAL_PORT_SLEEP_ON_NOTHING_RECEIVED 2
#define SERIAL_PORT_MAX_ERROR_BEFORE_KILL 20

class serial: public mlink
{
public:
    //construct and destruct
    serial(const std::string& port,
           const std::string& baudrate,
           bool flowcontrol,
           link_info info_,
           queue<std::pair<mlink*, mavlink_message_t>>* qMavIn);
    ~serial();

    //override virtuals from mlink
    void runWriteThread();
    void runReadThread();

private:
    //Callbacks for async send/recv
    void handleReceiveFrom(const boost::system::error_code& error,
                           size_t bytes_recvd);
    void handleSendTo(const boost::system::error_code& error,
                      size_t bytes_recvd);

    mavlink_message_t getMavMsg();

    boost::asio::io_service io_service_;
    boost::asio::serial_port port_;

    int errorcount = 0;

    //takes message, puts onto buff and calls send
    void processAndSend(mavlink_message_t *msgToConvert);

    //Actually sends
    void send(uint8_t *buf, std::size_t buf_size);
};

#endif
