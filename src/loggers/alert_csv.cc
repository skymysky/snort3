/*
** Copyright (C) 2014 Cisco and/or its affiliates. All rights reserved.
** Copyright (C) 2002-2013 Sourcefire, Inc.
** Copyright (C) 1998-2002 Martin Roesch <roesch@sourcefire.com>
** Copyright (C) 2001 Brian Caswell <bmc@mitre.org>
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License Version 2 as
** published by the Free Software Foundation.  You may not use, modify or
** distribute this program under any other version of the GNU General
** Public License.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>

#include <string>

#include "framework/logger.h"
#include "framework/module.h"
#include "decode.h"
#include "parser.h"
#include "snort_debug.h"
#include "mstring.h"
#include "util.h"
#include "log.h"
#include "snort.h"
#include "sf_textlog.h"
#include "log_text.h"

#define LOG_BUFFER (4*K_BYTES)

static THREAD_LOCAL TextLog* csv_log;

using namespace std;

//-------------------------------------------------------------------------
// module stuff
//-------------------------------------------------------------------------

static const char* csv_range =
    "timestamp | gid | sid | rev | msg | proto | "
    "src_addr | dst_addr | src_port | dst_port | "
    "eth_src | eth_dst | eth_type | eth_len | "
    "ttl | tos | id | ip_len | dgm_len | "
    "icmp_type | icmp_code | icmp_id | icmp_seq"
    "tcp_flags | tcp_seq | tcp_ack | tcp_len | tcp_win | "
    "udp_len";

static const char* csv_deflt =
    "timestamp gid sid rev src_addr src_port dst_addr dst_port";

static const Parameter csv_params[] =
{
    // FIXIT provide PT_FILE and PT_PATH and enforce no
    // path chars in file (outputs file must be in instance dir)
    { "file", Parameter::PT_STRING, nullptr, "stdout",
      "name of alert file" },

    { "csv", Parameter::PT_MULTI, csv_range, csv_deflt,
      "selected fields will be output in given order left to right" },

    { "limit", Parameter::PT_INT, "0:", "0",
      "set limit (0 is unlimited)" },

    // FIXIT provide PT_UNITS that converts to multiplier automatically
    { "units", Parameter::PT_ENUM, "B | K | M | G", "B",
      "bytes | KB | MB | GB" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

class CsvModule : public Module
{
public:
    CsvModule() : Module("alert_csv", csv_params) { };
    bool set(const char*, Value&, SnortConfig*);
    bool begin(const char*, int, SnortConfig*);
    bool end(const char*, int, SnortConfig*);

public:
    string file;
    string csvargs;
    unsigned long limit;
    unsigned units;
};

bool CsvModule::set(const char*, Value& v, SnortConfig*)
{
    if ( v.is("file") )
        file = v.get_string();

    else if ( v.is("csv") )
        csvargs = SnortStrdup(v.get_string());

    else if ( v.is("limit") )
        limit = v.get_long();

    else if ( v.is("units") )
        units = v.get_long();

    else
        return false;

    return true;
}

bool CsvModule::begin(const char*, int, SnortConfig*)
{
    file = "stdout";
    limit = 0;
    units = 0;
    csvargs = csv_deflt;
    return true;
}

bool CsvModule::end(const char*, int, SnortConfig*)
{
    while ( units-- )
        limit *= 1024;

    return true;
}

//-------------------------------------------------------------------------
// logger stuff
//-------------------------------------------------------------------------

class CsvLogger : public Logger {
public:
    CsvLogger(CsvModule*);
    ~CsvLogger();

    void open();
    void close();

    void alert(Packet*, const char* msg, Event*);

public:
    string file;
    unsigned long limit;
    char** args;
    int numargs;
};


CsvLogger::CsvLogger(CsvModule* m)
{
    file = m->file;
    limit = m->limit;
    args = mSplit(m->csvargs.c_str(), " \n\t", 0, &numargs, 0);
}

CsvLogger::~CsvLogger()
{
    mSplitFree(&args, numargs);
}

void CsvLogger::open()
{
    csv_log = TextLog_Init(file.c_str(), LOG_BUFFER, limit);
}

void CsvLogger::close()
{
    if ( csv_log )
        TextLog_Term(csv_log);
}

void CsvLogger::alert(Packet *p, const char *msg, Event *event)
{
    int num;
    char *type;
    char tcpFlags[9];

    assert(p);

    // TBD an enum would be an improvement here
    for (num = 0; num < numargs; num++)
    {
        type = args[num];

        if (!strcasecmp("timestamp", type))
        {
            LogTimeStamp(csv_log, p);
        }
        else if (!strcasecmp("gid", type))
        {
            if (event != NULL)
                TextLog_Print(csv_log, "%lu",  (unsigned long) event->sig_info->generator);
        }
        else if (!strcasecmp("sid", type))
        {
            if (event != NULL)
                TextLog_Print(csv_log, "%lu",  (unsigned long) event->sig_info->id);
        }
        else if (!strcasecmp("rev", type))
        {
            if (event != NULL)
                TextLog_Print(csv_log, "%lu",  (unsigned long) event->sig_info->rev);
        }
        else if (!strcasecmp("msg", type))
        {
            TextLog_Quote(csv_log, msg);  /* Don't fatal */
        }
        else if (!strcasecmp("proto", type))
        {
            if (IPH_IS_VALID(p))
            {
                switch (GET_IPH_PROTO(p))
                {
                    case IPPROTO_UDP:
                        TextLog_Puts(csv_log, "UDP");
                        break;
                    case IPPROTO_TCP:
                        TextLog_Puts(csv_log, "TCP");
                        break;
                    case IPPROTO_ICMP:
                        TextLog_Puts(csv_log, "ICMP");
                        break;
                    default:
                        break;
                }
            }
        }
        else if (!strcasecmp("eth_src", type))
        {
            if (p->eh != NULL)
            {
                TextLog_Print(csv_log, "%02X:%02X:%02X:%02X:%02X:%02X", p->eh->ether_src[0],
                        p->eh->ether_src[1], p->eh->ether_src[2], p->eh->ether_src[3],
                        p->eh->ether_src[4], p->eh->ether_src[5]);
            }
        }
        else if (!strcasecmp("eth_dst", type))
        {
            if (p->eh != NULL)
            {
                TextLog_Print(csv_log, "%02X:%02X:%02X:%02X:%02X:%02X", p->eh->ether_dst[0],
                        p->eh->ether_dst[1], p->eh->ether_dst[2], p->eh->ether_dst[3],
                        p->eh->ether_dst[4], p->eh->ether_dst[5]);
            }
        }
        else if (!strcasecmp("eth_type", type))
        {
            if (p->eh != NULL)
                TextLog_Print(csv_log, "0x%X", ntohs(p->eh->ether_type));
        }
        else if (!strcasecmp("eth_len", type))
        {
            if (p->eh != NULL)
                TextLog_Print(csv_log, "0x%X", p->pkth->pktlen);
        }
        else if (!strcasecmp("udp_len", type))
        {
            if (p->udph != NULL)
                TextLog_Print(csv_log, "%d", ntohs(p->udph->uh_len));
        }
        else if (!strcasecmp("src_port", type))
        {
            if (IPH_IS_VALID(p))
            {
                switch (GET_IPH_PROTO(p))
                {
                    case IPPROTO_UDP:
                    case IPPROTO_TCP:
                        TextLog_Print(csv_log, "%d", p->sp);
                        break;
                    default:
                        break;
                }
            }
        }
        else if (!strcasecmp("dst_port", type))
        {
            if (IPH_IS_VALID(p))
            {
                switch (GET_IPH_PROTO(p))
                {
                    case IPPROTO_UDP:
                    case IPPROTO_TCP:
                        TextLog_Print(csv_log, "%d", p->dp);
                        break;
                    default:
                        break;
                }
            }
        }
        else if (!strcasecmp("src_addr", type))
        {
            if (IPH_IS_VALID(p))
                TextLog_Puts(csv_log, inet_ntoa(GET_SRC_ADDR(p)));
        }
        else if (!strcasecmp("dst_addr", type))
        {
            if (IPH_IS_VALID(p))
                TextLog_Puts(csv_log, inet_ntoa(GET_DST_ADDR(p)));
        }
        else if (!strcasecmp("icmp_type", type))
        {
            if (p->icmph != NULL)
                TextLog_Print(csv_log, "%d", p->icmph->type);
        }
        else if (!strcasecmp("icmp_code", type))
        {
            if (p->icmph != NULL)
                TextLog_Print(csv_log, "%d", p->icmph->code);
        }
        else if (!strcasecmp("icmp_id", type))
        {
            if (p->icmph != NULL)
                TextLog_Print(csv_log, "%d", ntohs(p->icmph->s_icmp_id));
        }
        else if (!strcasecmp("icmp_seq", type))
        {
            if (p->icmph != NULL)
                TextLog_Print(csv_log, "%d", ntohs(p->icmph->s_icmp_seq));
        }
        else if (!strcasecmp("ttl", type))
        {
            if (IPH_IS_VALID(p))
                TextLog_Print(csv_log, "%d", GET_IPH_TTL(p));
        }
        else if (!strcasecmp("tos", type))
        {
            if (IPH_IS_VALID(p))
                TextLog_Print(csv_log, "%d", GET_IPH_TOS(p));
        }
        else if (!strcasecmp("id", type))
        {
            if (IPH_IS_VALID(p))
            {
                TextLog_Print(csv_log, "%u", IS_IP6(p) ? ntohl(GET_IPH_ID(p))
                        : ntohs((uint16_t)GET_IPH_ID(p)));
            }
        }
        else if (!strcasecmp("ip_len", type))
        {
            if (IPH_IS_VALID(p))
                TextLog_Print(csv_log, "%d", GET_IPH_LEN(p) << 2);
        }
        else if (!strcasecmp("dgm_len", type))
        {
            if (IPH_IS_VALID(p))
            {
                // XXX might cause a bug when IPv6 is printed?
                TextLog_Print(csv_log, "%d", ntohs(GET_IPH_LEN(p)));
            }
        }
        else if (!strcasecmp("tcp_seq", type))
        {
            if (p->tcph != NULL)
                TextLog_Print(csv_log, "0x%lX", (u_long)ntohl(p->tcph->th_seq));
        }
        else if (!strcasecmp("tcp_ack", type))
        {
            if (p->tcph != NULL)
                TextLog_Print(csv_log, "0x%lX", (u_long)ntohl(p->tcph->th_ack));
        }
        else if (!strcasecmp("tcp_len", type))
        {
            if (p->tcph != NULL)
                TextLog_Print(csv_log, "%d", TCP_OFFSET(p->tcph) << 2);
        }
        else if (!strcasecmp("tcp_win", type))
        {
            if (p->tcph != NULL)
                TextLog_Print(csv_log, "0x%X", ntohs(p->tcph->th_win));
        }
        else if (!strcasecmp("tcp_flags",type))
        {
            if (p->tcph != NULL)
            {
                CreateTCPFlagString(p, tcpFlags);
                TextLog_Print(csv_log, "%s", tcpFlags);
            }
        }

        if (num < numargs - 1)
            TextLog_Putc(csv_log, ',');
    }

    TextLog_NewLine(csv_log);
    TextLog_Flush(csv_log);
}

//-------------------------------------------------------------------------
// api stuff
//-------------------------------------------------------------------------

static Module* mod_ctor()
{ return new CsvModule; }

static void mod_dtor(Module* m)
{ delete m; }

static Logger* csv_ctor(SnortConfig*, Module* mod)
{ return new CsvLogger((CsvModule*)mod); }

static void csv_dtor(Logger* p)
{ delete p; }

static LogApi csv_api
{
    {
        PT_LOGGER,
        "alert_csv",
        LOGAPI_PLUGIN_V0,
        0,
        mod_ctor,
        mod_dtor
    },
    OUTPUT_TYPE_FLAG__ALERT,
    csv_ctor,
    csv_dtor
};

#ifdef BUILDING_SO
SO_PUBLIC const BaseApi* snort_plugins[] =
{
    &csv_api.base,
    nullptr
};
#else
const BaseApi* alert_csv = &csv_api.base;
#endif

