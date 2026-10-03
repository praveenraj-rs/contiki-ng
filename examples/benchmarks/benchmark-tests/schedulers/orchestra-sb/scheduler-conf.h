/* Orchestra with a sender-based unicast slotframe (RPL Classic storing mode) */
#define ORCHESTRA_CONF_RULES { &eb_per_time_source, \
                               &unicast_per_neighbor_rpl_storing, \
                               &default_common }
#define ORCHESTRA_CONF_UNICAST_SENDER_BASED 1
