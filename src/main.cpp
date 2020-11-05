/* CMAVNode
 * Monash UAS
 */

#include <boost/program_options.hpp>
#include <string>
#include <vector>
#include <boost/thread/thread.hpp>
#include <algorithm>
#include <ostream>
#include <boost/bind.hpp>
#include <boost/ref.hpp>
#include <iomanip>

// CMAVNode headers
#include "mlink.h"
#include "asyncsocket.h"
#include "serial.h"
#include "exception.h"
#include "shell.h"
#include "configfile.h"

//Periodic function timings
#define MAIN_LOOP_SLEEP_QUEUE_EMPTY_MS 10

// Functions in this file
boost::program_options::options_description add_program_options(std::string &filename, bool &shellen, bool &verbose);
int try_user_options(int argc, char** argv, boost::program_options::options_description desc);
void runMainLoop(std::vector<std::shared_ptr<mlink> > *links, queue<std::pair<mlink*, mavlink_message_t>>* qMavIn, bool &verbose);
void printLinkStats(std::vector<std::shared_ptr<mlink> > *links);
void printLinkQuality(std::vector<std::shared_ptr<mlink> > *links);
void getTargets(const mavlink_message_t* msg, int16_t &sysid, int16_t &compid);
void exitGracefully(int a);

bool exitMainLoop = false;

int main(int argc, char** argv)
{
    signal(SIGINT, exitGracefully);
    // Keep track of all known links
    std::vector<std::shared_ptr<mlink> > links;
    // Default mode selections
    bool shellen = true;
    bool verbose = false;

    std::string filename;
    boost::program_options::options_description desc = add_program_options(filename, shellen, verbose);

    int ret = try_user_options(argc, argv, desc);
    if (ret == 1)
        return 1; // Error
    else if (ret == -1)
        return 0; // Help option

    queue<std::pair<mlink*, mavlink_message_t>> qMavIn{MAV_INCOMING_LENGTH};
    ret = readConfigFile(filename, links, &qMavIn);
    if (links.size() == 0)
    {
        std::cout << "No Valid Links found" << std::endl;
        return 1; // Catch other errors
    }

    std::cout << "Command line arguments parsed succesfully." << std::endl;
    std::cout << "Links Initialized, routing loop starting." << std::endl;

    // Number the links
    for (uint16_t i = 0; i != links.size(); ++i)
    {
        links.at(i)->link_id = i;
    }

    // Run the shell thread
    boost::thread shell;
    if (shellen)
    {
        shell = boost::thread(runShell, boost::ref(exitMainLoop), boost::ref(links));
        // The boost::thread constructor implicitly binds runShell to &exitMainLoop and &links
    }

    // Start the main loop
    while (!exitMainLoop)
    {
        runMainLoop(&links, &qMavIn, verbose);
    }

    // Once the main loop is done, rejoin the shell thread
    if (shellen)
        shell.join();

    // Report successful exit from main()
    std::cout << "Links deallocated, stack unwound, exiting." << std::endl;
    return 0;
}

boost::program_options::options_description add_program_options(std::string &filename, bool &shellen, bool &verbose)
{
    boost::program_options::options_description desc("Options");
    desc.add_options()
    ("help", "Print help messages")
    ("file,f", boost::program_options::value<std::string>(&filename), "configuration file, usage: --file=path/to/file.conf")
    ("interface,i", boost::program_options::bool_switch(&shellen), "start in interactive mode with cmav shell")
    ("verbose,v", boost::program_options::bool_switch(&verbose), "verbose output including dropped packets");
    return desc;
}

int try_user_options(int argc, char** argv, boost::program_options::options_description desc)
{
    // Respond to the initial input (if any) provided by the user
    boost::program_options::variables_map vm;
    try
    {
        boost::program_options::store(
            boost::program_options::parse_command_line(argc, argv, desc), vm);
    }
    catch (boost::program_options::error& e)
    {
        std::cout << "ERROR: " << e.what() << std::endl;
        std::cerr << desc << std::endl;
        return 1; // Error in command line
    }

    // --help option
    if (vm.count("help"))
    {
        std::cout << "CMAVNode - Mavlink router" << std::endl
                  << desc << std::endl;
        return -1; // Help option selected
    }

    // Catch potential errors again
    try
    {
        boost::program_options::notify(vm);
    }
    catch (boost::program_options::error& e)
    {
        std::cerr << "ERROR: " << e.what() << std::endl;
        std::cerr << desc << std::endl;
        return 1; // Error in command line
    }

    // If no known option were given, return an error
    if( !vm.count("socket") && !vm.count("serial") && !vm.count("file") )
    {
        std::cerr << "Program cannot be run without arguments." << std::endl;
        std::cerr << desc << std::endl;
        return 1; // Error in command line
    }
    return 0; // No errors or help option detected

}

bool should_forward_message(const mavlink_message_t &msg, mlink *incoming_link, std::shared_ptr<mlink> *outgoing_link)
{

    // If the packet came from this link, don't bother
    if (outgoing_link->get() == incoming_link)
    {
        return false;
    }

    // Don't forward SiK radio info
    if (incoming_link->info.SiK_radio && msg.sysid == 51)
    {
        return false;
    }

    // If the current link being checked is designated to receive
    // from a non-zero system ID and that system ID isn't present on
    // this link, don't send on this link.
    if ((*outgoing_link)->info.output_only_from[0] != 0 &&
            std::find((*outgoing_link)->info.output_only_from.begin(),
                      (*outgoing_link)->info.output_only_from.end(),
                      msg.sysid) == (*outgoing_link)->info.output_only_from.end())
    {
        return false;
    }

    int16_t sysIDmsg = -1;
    int16_t compIDmsg = -1;
    getTargets(&msg, sysIDmsg, compIDmsg);
    if (sysIDmsg == -1)
    {
        return true;
    }
    if (compIDmsg == -1)
    {
        return true;
    }
    if (sysIDmsg == 0)
    {
        return true;
    }

    // if we get this far then the packet is routable; if we can't
    // find a route for it then we drop the message.
    if (!((*outgoing_link)->seenSysID(sysIDmsg)))
    {
        return false;
    }

    // TODO: should check sysid/compid combination has been seen, not
    // just sysid

    return true;
}

void runMainLoop(std::vector<std::shared_ptr<mlink> > *links, queue<std::pair<mlink*, mavlink_message_t>>* qMavIn, bool &verbose)
{
    // Iterate through each link
    for (auto incoming_link = links->begin(); incoming_link != links->end(); ++incoming_link)
    {
        // Clear out dead links
        (*incoming_link)->checkForDeadSysID();
    }

    // Gets run in a while loop once links are setup
    std::pair<mlink*, mavlink_message_t> p;
    if (!qMavIn->pop(&p)) {
        exitMainLoop = true;
        return;
    }

    mlink* const incoming_link = p.first;
    const mavlink_message_t msg = p.second;

    // Determine the correct target system ID for this message
    int16_t sysIDmsg = -1;
    int16_t compIDmsg = -1;
    getTargets(&msg, sysIDmsg, compIDmsg);


    // Iterate through each link to send to the correct target
    for (auto outgoing_link = links->begin(); outgoing_link != links->end(); ++outgoing_link)
    {
        // mavlink routing.  See comment in MAVLink_routing.cpp
        // for logic
        if (!should_forward_message(msg, incoming_link, &(*outgoing_link)))
        {
            continue;
        }

        // Provided nothing else has failed and the link is up, add the
        // message to the outgoing queue.
        if ((*outgoing_link)->up)
        {
            (*outgoing_link)->qAddOutgoing(msg);
        }
        else if (verbose)
        {
            std::cout << "Packet dropped from sysID: " << (int)msg.sysid
                      << " msgID: " << (int)msg.msgid
                      << " target system: " << (int)sysIDmsg
                      << " link name: " << incoming_link->info.link_name << std::endl;
        }
    }
}

void printLinkStats(std::vector<std::shared_ptr<mlink> > *links)
{
    std::cout << "---------------------------------------------------------------" << std::endl;
    // Print stats for each known link
    for (auto curr_link = links->begin(); curr_link != links->end(); ++curr_link)
    {
        std::ostringstream buffer;

        buffer << "Link: " << (*curr_link)->link_id << " "
               << (*curr_link)->info.link_name << " ";
        if ((*curr_link)->is_kill)
        {
            buffer << "DEAD ";
        }
        else if ((*curr_link)->up)
        {
            buffer << "UP ";
        }
        else
        {
            buffer << "DOWN ";
        }

        buffer << "Received: " << (*curr_link)->totalPacketCount << " "
               << "Sent: " << (*curr_link)->totalPacketSent << " "
               << "Systems on link: ";

        std::map<uint8_t, mlink::packet_stats>* sysID_map = &((*curr_link)->sysID_stats);

        for(auto iter = sysID_map->begin(); iter != sysID_map->end(); iter++)
        {
            buffer << (int)iter->first << " ";
        }

        buffer << "InQueue: " << (*curr_link)->in_counter.get();
        buffer << " OutQueue: " << (*curr_link)->out_counter.get();

        std::cout << buffer.str() << std::endl;
    }
    std::cout << "---------------------------------------------------------------" << std::endl;
}

void printLinkQuality(std::vector<std::shared_ptr<mlink> > *links)
{
    // Create a line of link quality
    std::ostringstream buffer;
    for (auto curr_link = links->begin(); curr_link != links->end(); ++curr_link)
    {
        buffer << "\nLink: " << (*curr_link)->link_id
               << "   (" << (*curr_link)->info.link_name << ")\n";

        // Only print radio stats when the link is connected to a SiK radio
        if ((*curr_link)->info.SiK_radio)
        {
            buffer  << std::setw(17)
                    << "Link delay: "<< std::setw(5) << (*curr_link)->link_quality.link_delay << " s\n"
                    << std::setw(17)
                    << "Local RSSI: " << std::setw(5) << (*curr_link)->link_quality.local_rssi
                    << std::setw(23)
                    << "Remote RSSI: " << std::setw(5) << (*curr_link)->link_quality.remote_rssi << "\n"
                    << std::setw(17)
                    << "Local noise: " << std::setw(5) << (*curr_link)->link_quality.local_noise
                    << std::setw(23)
                    << "Remote noise: " << std::setw(5) << (*curr_link)->link_quality.remote_noise << "\n"
                    << std::setw(17)
                    << "RX errors: " << std::setw(5) << (*curr_link)->link_quality.rx_errors
                    << std::setw(23)
                    << "Corrected packets: " << std::setw(5) << (*curr_link)->link_quality.corrected_packets << "\n"
                    << std::setw(17)
                    << "TX buffer: " << std::setw(5) << (*curr_link)->link_quality.tx_buffer << "%\n\n";
        }
        if ((*curr_link)->sysID_stats.size() != 0)
            buffer << std::setw(15) <<"System ID"
                   << std::setw(19) <<"Packets Lost"
                   << std::setw(19) <<"Packets Dropped"
                   << std::setw(19) <<"Packet Loss %" << "\n";
        for (auto iter = (*curr_link)->sysID_stats.begin(); iter != (*curr_link)->sysID_stats.end(); ++iter)
        {
            if ((*curr_link)->info.SiK_radio && iter->first == 51)
            {
                buffer << std::setw(11) << "(SiK)"
                       << std::setw(4) << (int)iter->first;
            }
            else
            {
                buffer << std::setw(15) << (int)iter->first;
            }
            buffer << std::setw(19) << iter->second.packets_lost
                   << std::setw(19) << iter->second.packets_dropped
                   << std::setw(19) << iter->second.packet_loss_percent << "\n";

        }
    }
    std::cout << "\n~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~"
              << buffer.str()
              <<   "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~" << std::endl;

}


void getTargets(const mavlink_message_t* msg, int16_t &sysid, int16_t &compid)
{
    /* --------METHOD TAKEN FROM ARDUPILOT ROUTING LOGIC CODE ------------*/
    // unfortunately the targets are not in a consistent position in
    // the packets, so we need a switch. Using the single element
    // extraction functions (which are inline) makes this a bit faster
    // than it would otherwise be.
    // This list of messages below was extracted using:
    //
    // cat ardupilotmega/*h common/*h|egrep
    // 'get_target_system|get_target_component' |grep inline | cut
    // -d'(' -f1 | cut -d' ' -f4 > /tmp/targets.txt
    //
    // TODO: we should write a python script to extract this list
    // properly

    switch (msg->msgid)
    {
    // these messages only have a target system
    case MAVLINK_MSG_ID_CAMERA_FEEDBACK:
        sysid = mavlink_msg_camera_feedback_get_target_system(msg);
        break;
    case MAVLINK_MSG_ID_CAMERA_STATUS:
        sysid = mavlink_msg_camera_status_get_target_system(msg);
        break;
    case MAVLINK_MSG_ID_CHANGE_OPERATOR_CONTROL:
        sysid = mavlink_msg_change_operator_control_get_target_system(msg);
        break;
    case MAVLINK_MSG_ID_SET_MODE:
        sysid = mavlink_msg_set_mode_get_target_system(msg);
        break;
    case MAVLINK_MSG_ID_SET_GPS_GLOBAL_ORIGIN:
        sysid = mavlink_msg_set_gps_global_origin_get_target_system(msg);
        break;

    // these support both target system and target component
    case MAVLINK_MSG_ID_DIGICAM_CONFIGURE:
        sysid  = mavlink_msg_digicam_configure_get_target_system(msg);
        compid = mavlink_msg_digicam_configure_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_DIGICAM_CONTROL:
        sysid  = mavlink_msg_digicam_control_get_target_system(msg);
        compid = mavlink_msg_digicam_control_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_FENCE_FETCH_POINT:
        sysid  = mavlink_msg_fence_fetch_point_get_target_system(msg);
        compid = mavlink_msg_fence_fetch_point_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_FENCE_POINT:
        sysid  = mavlink_msg_fence_point_get_target_system(msg);
        compid = mavlink_msg_fence_point_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_MOUNT_CONFIGURE:
        sysid  = mavlink_msg_mount_configure_get_target_system(msg);
        compid = mavlink_msg_mount_configure_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_MOUNT_CONTROL:
        sysid  = mavlink_msg_mount_control_get_target_system(msg);
        compid = mavlink_msg_mount_control_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_MOUNT_STATUS:
        sysid  = mavlink_msg_mount_status_get_target_system(msg);
        compid = mavlink_msg_mount_status_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_RALLY_FETCH_POINT:
        sysid  = mavlink_msg_rally_fetch_point_get_target_system(msg);
        compid = mavlink_msg_rally_fetch_point_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_RALLY_POINT:
        sysid  = mavlink_msg_rally_point_get_target_system(msg);
        compid = mavlink_msg_rally_point_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_SET_MAG_OFFSETS:
        sysid  = mavlink_msg_set_mag_offsets_get_target_system(msg);
        compid = mavlink_msg_set_mag_offsets_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_COMMAND_INT:
        sysid  = mavlink_msg_command_int_get_target_system(msg);
        compid = mavlink_msg_command_int_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_COMMAND_LONG:
        sysid  = mavlink_msg_command_long_get_target_system(msg);
        compid = mavlink_msg_command_long_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_FILE_TRANSFER_PROTOCOL:
        sysid  = mavlink_msg_file_transfer_protocol_get_target_system(msg);
        compid = mavlink_msg_file_transfer_protocol_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_GPS_INJECT_DATA:
        sysid  = mavlink_msg_gps_inject_data_get_target_system(msg);
        compid = mavlink_msg_gps_inject_data_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_LOG_ERASE:
        sysid  = mavlink_msg_log_erase_get_target_system(msg);
        compid = mavlink_msg_log_erase_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_LOG_REQUEST_DATA:
        sysid  = mavlink_msg_log_request_data_get_target_system(msg);
        compid = mavlink_msg_log_request_data_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_LOG_REQUEST_END:
        sysid  = mavlink_msg_log_request_end_get_target_system(msg);
        compid = mavlink_msg_log_request_end_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_LOG_REQUEST_LIST:
        sysid  = mavlink_msg_log_request_list_get_target_system(msg);
        compid = mavlink_msg_log_request_list_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_MISSION_ACK:
        sysid  = mavlink_msg_mission_ack_get_target_system(msg);
        compid = mavlink_msg_mission_ack_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_MISSION_CLEAR_ALL:
        sysid  = mavlink_msg_mission_clear_all_get_target_system(msg);
        compid = mavlink_msg_mission_clear_all_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_MISSION_COUNT:
        sysid  = mavlink_msg_mission_count_get_target_system(msg);
        compid = mavlink_msg_mission_count_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_MISSION_ITEM:
        sysid  = mavlink_msg_mission_item_get_target_system(msg);
        compid = mavlink_msg_mission_item_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_MISSION_ITEM_INT:
        sysid  = mavlink_msg_mission_item_int_get_target_system(msg);
        compid = mavlink_msg_mission_item_int_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_MISSION_REQUEST:
        sysid  = mavlink_msg_mission_request_get_target_system(msg);
        compid = mavlink_msg_mission_request_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_MISSION_REQUEST_LIST:
        sysid  = mavlink_msg_mission_request_list_get_target_system(msg);
        compid = mavlink_msg_mission_request_list_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_MISSION_REQUEST_PARTIAL_LIST:
        sysid  = mavlink_msg_mission_request_partial_list_get_target_system(msg);
        compid = mavlink_msg_mission_request_partial_list_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_MISSION_SET_CURRENT:
        sysid  = mavlink_msg_mission_set_current_get_target_system(msg);
        compid = mavlink_msg_mission_set_current_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_MISSION_WRITE_PARTIAL_LIST:
        sysid  = mavlink_msg_mission_write_partial_list_get_target_system(msg);
        compid = mavlink_msg_mission_write_partial_list_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_PARAM_REQUEST_LIST:
        sysid  = mavlink_msg_param_request_list_get_target_system(msg);
        compid = mavlink_msg_param_request_list_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_PARAM_REQUEST_READ:
        sysid  = mavlink_msg_param_request_read_get_target_system(msg);
        compid = mavlink_msg_param_request_read_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_PARAM_SET:
        sysid  = mavlink_msg_param_set_get_target_system(msg);
        compid = mavlink_msg_param_set_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_PING:
        sysid  = mavlink_msg_ping_get_target_system(msg);
        compid = mavlink_msg_ping_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_RC_CHANNELS_OVERRIDE:
        sysid  = mavlink_msg_rc_channels_override_get_target_system(msg);
        compid = mavlink_msg_rc_channels_override_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_REQUEST_DATA_STREAM:
        sysid  = mavlink_msg_request_data_stream_get_target_system(msg);
        compid = mavlink_msg_request_data_stream_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_SAFETY_SET_ALLOWED_AREA:
        sysid  = mavlink_msg_safety_set_allowed_area_get_target_system(msg);
        compid = mavlink_msg_safety_set_allowed_area_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_SET_ATTITUDE_TARGET:
        sysid  = mavlink_msg_set_attitude_target_get_target_system(msg);
        compid = mavlink_msg_set_attitude_target_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_SET_POSITION_TARGET_GLOBAL_INT:
        sysid  = mavlink_msg_set_position_target_global_int_get_target_system(msg);
        compid = mavlink_msg_set_position_target_global_int_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_SET_POSITION_TARGET_LOCAL_NED:
        sysid  = mavlink_msg_set_position_target_local_ned_get_target_system(msg);
        compid = mavlink_msg_set_position_target_local_ned_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_V2_EXTENSION:
        sysid  = mavlink_msg_v2_extension_get_target_system(msg);
        compid = mavlink_msg_v2_extension_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_GIMBAL_REPORT:
        sysid  = mavlink_msg_gimbal_report_get_target_system(msg);
        compid = mavlink_msg_gimbal_report_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_GIMBAL_CONTROL:
        sysid  = mavlink_msg_gimbal_control_get_target_system(msg);
        compid = mavlink_msg_gimbal_control_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_GIMBAL_TORQUE_CMD_REPORT:
        sysid  = mavlink_msg_gimbal_torque_cmd_report_get_target_system(msg);
        compid = mavlink_msg_gimbal_torque_cmd_report_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_REMOTE_LOG_DATA_BLOCK:
        sysid  = mavlink_msg_remote_log_data_block_get_target_system(msg);
        compid = mavlink_msg_remote_log_data_block_get_target_component(msg);
        break;
    case MAVLINK_MSG_ID_REMOTE_LOG_BLOCK_STATUS:
        sysid  = mavlink_msg_remote_log_block_status_get_target_system(msg);
        compid = mavlink_msg_remote_log_block_status_get_target_component(msg);
        break;
    }
}

void exitGracefully(int a)
{
    std::cout << "Exit code " << a << std::endl;
    std::cout << "SIGINT caught, deconstructing links and exiting" << std::endl;
    exitMainLoop = true;
}
