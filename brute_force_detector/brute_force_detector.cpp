/**
 * \file brute_force_detector.cpp
 * \brief Brute force detector module
 * \author Vaclav Pacholik <xpacho03@stud.fit.vutbr.cz || vaclavpacholik@gmail.com>
 * \date 2014
 */

/*
 * Copyright (C) 2014 CESNET
 *
 * LICENSE TERMS
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of the Company nor the names of its contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * ALTERNATIVELY, provided that this notice is retained in full, this
 * product may be distributed under the terms of the GNU General Public
 * License (GPL) version 2 or later, in which case the provisions
 * of the GPL apply INSTEAD OF those given above.
 *
 * This software is provided ``as is'', and any express or implied
 * warranties, including, but not limited to, the implied warranties of
 * merchantability and fitness for a particular purpose are disclaimed.
 * In no event shall the company or contributors be liable for any
 * direct, indirect, incidental, special, exemplary, or consequential
 * damages (including, but not limited to, procurement of substitute
 * goods or services; loss of use, data, or profits; or business
 * interruption) however caused and on any theory of liability, whether
 * in contract, strict liability, or tort (including negligence or
 * otherwise) arising in any way out of the use of this software, even
 * if advised of the possibility of such damage.
 *
 */
#ifdef HAVE_CONFIG_H
#include "../config.h" // include autogenerated config
#endif

#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <getopt.h>
#include <time.h>

#include <libtrap/trap.h>
#include <unirec/unirec.h>

#include <iostream>
#include <string>
#include <map>
#include <unistd.h>

#include "record.h"
#include "config.h"
#include "host.h"
#include "sender.h"
#include "brute_force_detector.h"
#include "whitelist.h"
#include <locale>
#include <sys/time.h>
#include <iomanip>
#ifdef __cplusplus
extern "C" {
#endif
#include "fields.h"
#ifdef __cplusplus
}
#endif

using namespace std;

UR_FIELDS(
  // BASIC_FLOW
  ipaddr SRC_IP,      // Source address of a flow
  ipaddr DST_IP,      // Destination address of a flow
  uint16 SRC_PORT,    // Source transport-layer port
  uint16 DST_PORT,    // Destination transport-layer port
  uint8 PROTOCOL,     // L4 protocol (TCP, UDP, ICMP, etc.)
  uint32 PACKETS,     // Number of packets in a flow or in an interval
  uint64 BYTES,       // Number of bytes in a flow or in an interval
  time TIME_FIRST,    // Timestamp of the first packet of a flow
  time TIME_LAST,     // Timestamp of the last packet of a flow
  uint8 TCP_FLAGS,    // TCP flags of a flow (logical OR over TCP flags field of all packets)
)

/* ************************************************************************* */
// Struct with information about module
trap_module_info_t *module_info = nullptr;


#define MODULE_BASIC_INFO(BASIC) \
  BASIC("brute_force_detector","A module used for detecting brute force dictionary attacks on SSH, RDP and Telnet protocols. For detection is used window with N last flows and if threshold of a suspicious flows (packet and byte range is checked) is reached, alarm is triggered.",1,1)

#define MODULE_PARAMS(PARAM) \
    PARAM('S', "SSH", "Set detection mode to SSH.", no_argument, "none") \
    PARAM('R', "RDP", "Set detection mode to RDP.", no_argument, "none") \
    PARAM('T', "TELNET", "Set detection mode to TELNET.", no_argument, "none") \
    PARAM('c', "config", "Specify configuration file. Signal SIGUSR1 can be used for reload configuration file. (not required)", required_argument, "string") \
    PARAM('w', "whitelist", "Specify whitelist file. Signal SIGUSR2 can be used for whitelist reload. (not required)", required_argument, "string") \
    PARAM('W', "verbose", "Set whitelist parser to verbose mode.", no_argument, "none")

static int stop = 0;

  // TODO consider changing globals
TelnetServerProfileMap TELNETRecord::TSPMap;
Whitelist whitelist;

void signalHandler(int signal)
{
    if(signal == SIGTERM || signal == SIGINT)
    {
        stop = 1;
        trap_terminate();
    }
    else if(signal == SIGUSR1)
    {
        Config::getInstance().reloadConfig();
    }
    else if(signal == SIGUSR2)
    {
        if(!whitelist.isLockedForConfigurationReload())
            whitelist.reloadWhitelist();
        else
            alarm(1); // cannot reload now.. wait
    }
    else if(signal == SIGALRM)
    {
        if(!whitelist.isLockedForConfigurationReload())
            whitelist.reloadWhitelist();
        else
            alarm(1); // wait another second
    }
}

bool checkForTimeout(ur_time_t oldTime, ur_time_t timer, ur_time_t actualTime)
{
	return oldTime + timer <= actualTime;
}


void printFlowPercent(uint64_t b, uint64_t p, std::string comment /* optional, implicitly set to "" */)
{
	if (b && p) {
		ios::fmtflags f(cout.flags());
		cout << " ("
			 << std::fixed << std::setprecision(2)
			 << 100.0 / b * p
			 << "%" << comment << ")";
		cout.flags(f);
	}
	/*
	else {
		cerr << "Attempted division by zero in printFlowPercent." << endl;
	}
	 // error output not necessary
	 */

}


int main(int argc, char **argv)
{
	getc(stdin); /// DEBUGGING
    // ***** TRAP initialization *****
    int ret;
    INIT_MODULE_INFO_STRUCT(MODULE_BASIC_INFO, MODULE_PARAMS)
    TRAP_DEFAULT_INITIALIZATION(argc, argv, *module_info);

    // ***** Capturing signals *****
#ifdef HAVE_SIGACTION
    struct sigaction sigAction{};

    sigAction.sa_handler = signalHandler;
    sigemptyset (&sigAction.sa_mask);
    sigAction.sa_flags = 0;

    // Prevent interruption of signal handler by another SIGTERM or SIGINT
    sigaddset(&sigAction.sa_mask, SIGTERM);
    sigaddset(&sigAction.sa_mask, SIGINT);
    sigaddset(&sigAction.sa_mask, SIGUSR1);
    sigaddset(&sigAction.sa_mask, SIGUSR2);
    sigaddset(&sigAction.sa_mask, SIGALRM);

    // register signal handler
    sigaction (SIGTERM, &sigAction, nullptr);
    sigaction (SIGINT , &sigAction, nullptr);
    sigaction (SIGUSR1, &sigAction, nullptr);
    sigaction (SIGUSR2, &sigAction, nullptr);
    sigaction (SIGALRM, &sigAction, nullptr);
#else
    signal(SIGTERM, signalHandler);
    signal(SIGINT,  signalHandler);
    signal(SIGUSR1, signalHandler);
    signal(SIGUSR2, signalHandler);
    signal(SIGALRM, signalHandler);
#endif

    // ***** Parsing non TRAP arguments *****
    signed char opt;
    char *configFilePath = nullptr;
    char *whitelistFilePath = nullptr;
    bool whitelistParserVerbose = false;
    bool RDP = false, SSH = false, TELNET = false;
    while((opt = TRAP_GETOPT(argc, argv, module_getopt_string, long_options)) != -1)
    {
        switch (opt)
        {
        case 'c': // config
            configFilePath = optarg;
            break;
        case 'w':
            whitelistFilePath = optarg;
            break;
        case 'W':
            whitelistParserVerbose = true;
            break;
        case 'R':
            RDP = true;
            break;
        case 'S':
            SSH = true;
            break;
        case 'T':
            TELNET = true;
            break;
        default:
            cerr << "Error: Invalid arguments.\n";
            FREE_MODULE_INFO_STRUCT(MODULE_BASIC_INFO, MODULE_PARAMS)
            return 3;
        }
    }

    if(!RDP && !SSH && !TELNET)
    {
        cerr << "Error: Detection mode is not set.\n";
        return 3;
    }

    // ***** Config init *****
    if(configFilePath != nullptr)
    {
        bool state = Config::getInstance().initFromFile(configFilePath);
        if(!state)
        {
            cerr << "Error: Cannot open configuration file \"" << configFilePath << "\".\n";
            FREE_MODULE_INFO_STRUCT(MODULE_BASIC_INFO, MODULE_PARAMS)
            return 5;
        }
    }

	// ***** Whitelist init *****
    if(whitelistFilePath != nullptr)
    {
        bool state = whitelist.init(whitelistFilePath, whitelistParserVerbose);
        if (!state)
        {
            cerr << "Error: Cannot open whitelist file.\n";
            FREE_MODULE_INFO_STRUCT(MODULE_BASIC_INFO, MODULE_PARAMS)
            return 5;
        }
    }

    // ***** Create UniRec template for input *****
    std::string unirecSpecifier = "SRC_IP,DST_IP,SRC_PORT,DST_PORT,PROTOCOL,PACKETS,BYTES,TIME_FIRST,TIME_LAST,TCP_FLAGS";

    char *errstr = nullptr;
    ur_template_t *tmplt = ur_create_input_template(0, unirecSpecifier.c_str(), &errstr);
    if (tmplt == nullptr) {
      cerr << "Error: Invalid UniRec specifier." << endl;
      if(errstr != nullptr){
        fprintf(stderr, "%s\n", errstr);
        free(errstr);
      }
      trap_finalize();
	  FREE_MODULE_INFO_STRUCT(MODULE_BASIC_INFO, MODULE_PARAMS);
      return 4;
    }

    // ***** Init UniRec template for sender *****

    bool senderState;
    auto sender = new Sender(&senderState);
    if(!senderState)
    {
        cerr << "Error: Invalid output UniRec specifier. Check sender.cpp file.\n";
        delete sender;
        ur_free_template(tmplt);
        trap_finalize();
        FREE_MODULE_INFO_STRUCT(MODULE_BASIC_INFO, MODULE_PARAMS)
        return 4;
    }


    // ***** Main processing loop *****
    SSHHostMap    sshHostMap;
    RDPHostMap    rdpHostMap;
    TELNETHostMap telnetHostMap;
    
    logInfo ssh("SSH");
	logInfo rdp("RDP");
	logInfo telnet("TELNET");

    ur_time_t timeOfLastReportCheck = 0;
    ur_time_t timeOfLastDeleteCheck = 0;
    ur_time_t timerForReportCheck = Config::getInstance().getGlobalTimerForReportCheck();
    ur_time_t timerForDeleteCheck = Config::getInstance().getGlobalTimerForDeleteCheck();

    while(!stop)
    {
        // Receive data from input interface (block until data are available)
        const void *data;
        uint16_t data_size;
        ret = TRAP_RECEIVE(0, data, data_size, tmplt);
        TRAP_DEFAULT_RECV_ERROR_HANDLING(ret, continue, break);

        // Check size of received data
        if(data_size < ur_rec_fixlen_size(tmplt))
        {
            if(data_size <= 1)
            {
                break; // End of data (used for testing purposes)
            }
            else
            {
                cerr << "Error: data with wrong size received (expected size: >= "
                     << ur_rec_fixlen_size(tmplt) << ", received size: "
                     << data_size << ")\n";
                break;
            }
        }

        // Skip non TCP flows
        if(ur_get(tmplt, data, F_PROTOCOL) != TCP_PROTOCOL_NUM) // TCP flows only
            continue;

        uint16_t dstPort = ur_get(tmplt, data, F_DST_PORT);
        uint16_t srcPort = ur_get(tmplt, data, F_SRC_PORT);

	    if(dstPort != TCP_SSH_PORT && dstPort != TCP_TELNET_PORT && dstPort != TCP_RDP_PORT
	       && srcPort != TCP_SSH_PORT && srcPort != TCP_TELNET_PORT && srcPort != TCP_RDP_PORT)
	        continue;

	    uint8_t direction;
        if(dstPort == TCP_SSH_PORT || dstPort == TCP_TELNET_PORT || dstPort == TCP_RDP_PORT)
            direction = FLOW_INCOMING_DIRECTION;
        else
            direction = FLOW_OUTGOING_DIRECTION;

	    // Process rest of new data
        IRecord::MatchStructure structure { // TODO proper name
				.flags = ur_get(tmplt, data, F_TCP_FLAGS),
				.packets = ur_get(tmplt, data, F_PACKETS),
				.bytes   = ur_get(tmplt, data, F_BYTES),
				.srcIp   = ur_get(tmplt, data, F_SRC_IP),
				.dstIp   = ur_get(tmplt, data, F_DST_IP),
				.srcPort = srcPort,
				.dstPort = dstPort,
				.flowFirstSeen = ur_get(tmplt, data, F_TIME_FIRST),
				.flowLastSeen  = ur_get(tmplt, data, F_TIME_LAST),
		};

        ret = 0;

        // *** SSH ***
        if(SSH && (dstPort == TCP_SSH_PORT || srcPort == TCP_SSH_PORT))
        {
            bool is_matched;
            SSHRecord *record;

            if(direction == FLOW_INCOMING_DIRECTION)
            {
                record = new SSHRecord(structure.dstIp, structure.flowLastSeen);
                is_matched = record->matchWithIncomingSignature(&structure, &whitelist);
                if(is_matched)
                    ssh.matchedIncomingFlows++;
                ssh.incomingFlows++;
            }
            else
            { // FLOW_OUTGOING_DIRECTIONstructu
                record = new SSHRecord(structure.srcIp, structure.flowLastSeen);
                is_matched = record->matchWithOutgoingSignature(&structure, &whitelist);
                if(is_matched)
                    ssh.matchedOutgoingFlows++;
                ssh.outgoingFlows++;
            }

            if(is_matched)
                ssh.matchedFlows++;
            ssh.flows++;

            SSHHost *host = sshHostMap.findHost(&structure, direction);

            is_matched = host->addRecord(record, &structure, direction);
            if(!is_matched)
                delete record;
            else
            {				// check for attack
                auto attackState = host->checkForAttack(structure.flowLastSeen);
                if(attackState != SSHHost::NO_ATTACK)
                {
                    if(attackState == SSHHost::NEW_ATTACK)
                        ret = sender->firstReport(host, TCP_SSH_PORT, structure.flowLastSeen, Config::getInstance().getSSHListThreshold());
                    else if(attackState == SSHHost::ATTACK_REPORT_WAIT || attackState == SSHHost::ATTACK_MIN_EVENTS_WAIT)
                    {
                        // waiting for report timeout or min events to report
                        // no action
                    }
                    else if(attackState == SSHHost::END_OF_ATTACK)
                    {
                        // clear list
                        host->clearAllRecords();
                        host->setNotReported();
                    }
                    else if(attackState == SSHHost::REPORT_END_OF_ATTACK)
                    {
                        // report and clear list
                        ret = sender->continuingReport(host, TCP_SSH_PORT, structure.flowLastSeen, true);

                        host->clearAllRecords();
                        host->setNotReported();
                    }
                    else if(attackState == SSHHost::ATTACK)
                    {
                        ret = sender->continuingReport(host, TCP_SSH_PORT, structure.flowLastSeen);
                    }
                }
            }
        }

        // *** RDP ***
        if(RDP && (dstPort == TCP_RDP_PORT || srcPort == TCP_RDP_PORT))
        {
            bool is_matched;
            RDPRecord *record;

            if(direction == FLOW_INCOMING_DIRECTION)
            {
                record = new RDPRecord(structure.dstIp, structure.flowLastSeen);
                is_matched = record->matchWithIncomingSignature(&structure, &whitelist);
                if(is_matched)
                    rdp.matchedIncomingFlows++;
                rdp.incomingFlows++;
            }
            else
            { // FLOW_OUTGOING_DIRECTION
                record = new RDPRecord(structure.srcIp, structure.flowLastSeen);
                is_matched = record->matchWithOutgoingSignature(&structure, &whitelist);
                if(is_matched)
                    rdp.matchedOutgoingFlows++;
                rdp.outgoingFlows++;
            }

            if(is_matched)
                rdp.matchedFlows++;
            rdp.flows++;

            RDPHost *host = rdpHostMap.findHost(&structure, direction);

            is_matched = host->addRecord(record, &structure, direction);
            if(!is_matched)
                delete record;
            else
            {					  // check for attack
                RDPHost::ATTACK_STATE attackState = host->checkForAttack(structure.flowLastSeen);
                if(attackState != RDPHost::NO_ATTACK)
                {
                    if(attackState == RDPHost::NEW_ATTACK)
                        ret = sender->firstReport(host, TCP_RDP_PORT, structure.flowLastSeen, Config::getInstance().getRDPListThreshold());
                    else if(attackState == RDPHost::ATTACK_REPORT_WAIT || attackState == RDPHost::ATTACK_MIN_EVENTS_WAIT)
                    {
                        // waiting for report timeout or min events to report
                        // no action
                    }
                    else if(attackState == RDPHost::END_OF_ATTACK)
                    {
                        host->clearAllRecords();
                        host->setNotReported();
                    }
                    else if(attackState == RDPHost::REPORT_END_OF_ATTACK)
                    {
                        // report and clear list
                        ret = sender->continuingReport(host, TCP_RDP_PORT, structure.flowLastSeen, true);

                        host->clearAllRecords();
                        host->setNotReported();
                    }
                    else if(attackState == RDPHost::ATTACK)
                    {
                        ret = sender->continuingReport(host, TCP_RDP_PORT, structure.flowLastSeen);
                    }
                }
            }
        }

        // *** TELNET ***
        if(TELNET && (dstPort == TCP_TELNET_PORT || srcPort == TCP_TELNET_PORT))
        {
            bool is_matched;
            TELNETRecord *record;

            if(direction == FLOW_INCOMING_DIRECTION)
            {
                record = new TELNETRecord(structure.dstIp, structure.flowLastSeen);
                is_matched = record->matchWithIncomingSignature(&structure, &whitelist);
                if(is_matched)
                    telnet.matchedIncomingFlows++;
                telnet.incomingFlows++;
            }
            else
            { // FLOW_OUTGOING_DIRECTION
                record = new TELNETRecord(structure.srcIp, structure.flowLastSeen);
                is_matched = record->matchWithOutgoingSignature(&structure, &whitelist);
                if(is_matched)
                    telnet.matchedOutgoingFlows++;
                telnet.outgoingFlows++;
            }

            if(is_matched)
                telnet.matchedFlows++;
            telnet.flows++;

            TELNETHost *host = telnetHostMap.findHost(&structure, direction);

            is_matched = host->addRecord(record, &structure, direction);
            if(!is_matched)
                delete record;
            else
            {					  // check for attack
                TELNETHost::ATTACK_STATE attackState = host->checkForAttack(structure.flowLastSeen);
                if(attackState != TELNETHost::NO_ATTACK)
                {
                    if(attackState == TELNETHost::NEW_ATTACK)
                        ret = sender->firstReport(host, TCP_TELNET_PORT, structure.flowLastSeen, Config::getInstance().getTELNETListThreshold());
                    else if(attackState == TELNETHost::ATTACK_REPORT_WAIT || attackState == TELNETHost::ATTACK_MIN_EVENTS_WAIT)
                    {
                        // waiting for report timeout or min events to report
                        // no action
                    }
                    else if(attackState == TELNETHost::END_OF_ATTACK)
                    {
                        host->clearAllRecords();
                        host->setNotReported();
                    }
                    else if(attackState == TELNETHost::REPORT_END_OF_ATTACK)
                    {
                        // report and clear list
                        ret = sender->continuingReport(host, TCP_TELNET_PORT, structure.flowLastSeen, true);

                        host->clearAllRecords();
                        host->setNotReported();
                    }
                    else if(attackState == TELNETHost::ATTACK)
                    {
                        ret = sender->continuingReport(host, TCP_TELNET_PORT, structure.flowLastSeen);
                    }
                }
            }
        }

        // check for timeout
        if(checkForTimeout(timeOfLastReportCheck, timerForReportCheck, structure.flowLastSeen))
        {
            timeOfLastReportCheck = structure.flowLastSeen;

            if(SSH)
                sshHostMap.checkForAttackTimeout(structure.flowLastSeen, sender);
            if(RDP)
                rdpHostMap.checkForAttackTimeout(structure.flowLastSeen, sender);
            if(TELNET)
                telnetHostMap.checkForAttackTimeout(structure.flowLastSeen, sender);
        }
        if(checkForTimeout(timeOfLastDeleteCheck, timerForDeleteCheck, structure.flowLastSeen))
        {
            timeOfLastDeleteCheck = structure.flowLastSeen;

            if(SSH)
                sshHostMap.deleteOldRecordAndHosts(structure.flowLastSeen);
            if(RDP)
                rdpHostMap.deleteOldRecordAndHosts(structure.flowLastSeen);
            if(TELNET)
                telnetHostMap.deleteOldRecordAndHosts(structure.flowLastSeen);
        }

	    // kontrola po odeslani
	    TRAP_DEFAULT_SEND_DATA_ERROR_HANDLING(ret, continue, break);

    } // ***** End of main processing loop *****

    // TODO how to pass Host Map base class to printLogInfo
    if(SSH)
    {
		ssh.printLogInfo();
		std::cout << "  Host Map Size: " << sshHostMap.size() << std::endl;
		sshHostMap.clear();
    }
    if(RDP)
    {
		rdp.printLogInfo();
		std::cout << "  Host Map Size: " << rdpHostMap.size() << std::endl;
	    rdpHostMap.clear();
    }
    if(TELNET)
    {
		telnet.printLogInfo();
		std::cout << "  Host Map Size: " << telnetHostMap.size() << std::endl;
	    telnetHostMap.clear();
    }

    TRAP_DEFAULT_FINALIZATION();
    ur_free_template(tmplt);
    ur_finalize();
    delete sender; // free output template
    FREE_MODULE_INFO_STRUCT(MODULE_BASIC_INFO, MODULE_PARAMS);
    return 0;
}
/*
void printHosts(IHostMap map)
{
	for(auto item : map)
	{

	}
}
*/
