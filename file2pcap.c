/*
	file2pcap
	Code by Martin Zeiser
	Started July 2009
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <libgen.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define __FAVOR_BSD
#include <netinet/in_systm.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h> // struct ip6_hdr
#include <netinet/tcp.h>

#include "file2pcap.h"
#include "ftp.h"
#include "http.h"
#include "http2.h"
#include "smtp.h"
#include "pop3.h"
#include "imap.h"

struct
{
	int PCAPMAGIC;
	const short major_version;
	short minor_version;
	int GMT;
	int timestamp_granularity;
	int entire_packet;
	int assume_ethernet;
} pcap_file_header = {0xA1B2C3D4, 2, 4, 0, 0, 65535, 1};

FILE *inFile, *outFile;

size_t packetLen4 = (sizeof(SRC_ETHER) - 1 + sizeof(DST_ETHER) - 1 + sizeof(PROTO_ETHER) - 1 + sizeof(struct ip) + sizeof(struct tcphdr));
size_t packetLen6 = (sizeof(SRC_ETHER) - 1 + sizeof(DST_ETHER) - 1 + sizeof(PROTO_ETHER6) - 1 + sizeof(struct ip6_hdr) + sizeof(struct tcphdr));
size_t etherLen = (sizeof(SRC_ETHER) - 1 + sizeof(DST_ETHER) - 1 + sizeof(PROTO_ETHER) - 1);

struct handover hoFtp;

/***************************************************************************************/

void usage()
{
	char *helptext = "\nfile2pcap - developed by Martin Zeiser(mzeiser@cisco.com)\n"
					 "Version: " VERSION "\n"
					 "Takes a file as input and creates a pcap showing that file being transferred between hosts\n"
					 "\nOptions:\n"
                     "-v vlan\t\t - vlan number\n"
					 "-e encoding\t\t0 - base64\n"
					 "\t\t\t1 - quoted printable\n"
					 "\t\t\t2 - UU encoding\n"
					 "\t\t\t[default: base64]\n"
					 "-m mode\r\n"
					 "\t\t\th - http GET (download)\r\n"
					 "\t\t\th2 - http2 GET (download)\r\n"
					 "\t\t\tH - http POST (upload)\r\n"
					 "\t\t\ts - smtp\r\n"
					 "\t\t\tp - pop3\r\n"
					 "\t\t\ti - imap\r\n"
					 "\t\t\tf - active ftp\r\n"
					 "\t\t\tF - passive ftp\r\n"
					 "\t\t\t[default: http GET]\n\n"
					 "-o outfile\t\toutput filename [default: <filename>-<protocol>.pcap]\n"
					 "-p port[:port]\t\tspecify [source and] destination port\n"
					 "\t\t\t-p 8080 will simulate a connection from a random port to port 8080\n"
					 "\t\t\t-p 1234:80 will simulate a connection from port 1234 to port 80\n"
					 "\t\t\t[default: use IANA assigned port for the protocol]\n\n"
					 "-6\t\t\tUse IPv6 instead of the default IPv4\n";
	char *usage = "\nUsage:\n"
				  "\t\t\tfile2pcap [options] infile\n";
	char *example = "Examples:\n"
					"\t\t\tfile2pcap malware.pdf\n"
					"\t\t\tfile2pcap -mshp malware.pdf\n"
					"\t\t\tfile2pcap -mH -p8080 malware.pdf\n"
					"\t\t\tfile2pcap -mi malware.pdf -o outfile.pcap\n";

	printf("%s\n", helptext);
	printf("%s\n", usage);
	printf("%s\n", example);

	exit(0);

	return;
}

/**********************************************************************************************************/
char *random_email(int src_or_dst, char **random_words_list)
{
	static char email[100];
	srand(getpid() * time(NULL) * (rand() + src_or_dst));
	// src_or_dst is 0 for src, 1 for dst
	if (src_or_dst == 0){
	snprintf(email, sizeof(email), "%s-%s@src_email.com", random_words_list[rand() % sizeof(random_words_list)],
			 random_words_list[rand() % sizeof(random_words_list)]);
	}
	if (src_or_dst == 1){
		snprintf(email, sizeof(email), "%s-%s@dst_email.com", random_words_list[rand() % sizeof(random_words_list)],
				random_words_list[rand() % sizeof(random_words_list)]);
	}
	return email;
}

char *random_host(char **random_words_list)
{
	static char host[100];
	srand(getpid() * time(NULL));
	snprintf(host, sizeof(host), "%s-%s.com", random_words_list[rand() % sizeof(random_words_list)],
			 random_words_list[rand() % sizeof(random_words_list)]);

	return host;
}

char **load_random_words(const char *filename, int count)
{
	const int LINE_MAX = 50;
	FILE *file;
	char **lines;
	char buf[LINE_MAX];
	int i, n;

	file = fopen(filename, "r");
	lines = calloc(count, sizeof(char *));
	for (n = 1; fgets(buf, LINE_MAX, file); n++)
	{
		buf[strcspn(buf, "\'")] = 0;
		buf[strcspn(buf, "\n")] = 0;
		if (n <= count)
		{
			lines[n - 1] = strdup(buf);
		}
		else
		{
			i = rand() % n;
			if (i < count)
			{
				free(lines[i]);
				lines[i] = strdup(buf);
			}
		}
	}
	fclose(file);

	return lines;
}

char *random_ipv4(int src_or_dst)
{
	static char IP4[15];
	// src_or_dst is 0 for src, 1 for dst
	srand(getpid() * time(NULL) + src_or_dst);
	snprintf(IP4, sizeof(IP4), "%d.%d.%d.%d", rand() & 0xFF,
			 rand() & 0xFF, rand() & 0xFF, rand() & 0xFF);

	return IP4;
}

unsigned short ipChecksum(unsigned short *ptr, int nbytes)
{
	register long sum = 0;
	register u_short answer;
	u_short oddbyte;

	while (nbytes > 1)
	{
		sum += *ptr++;
		nbytes -= 2;
	}

	if (nbytes == 1)
	{
		oddbyte = 0;
		*((u_char *)&oddbyte) = *(u_char *)ptr;
		sum += oddbyte;
	}

	sum = (sum >> 16) + (sum & 0xffff);
	sum += (sum >> 16);
	answer = ~sum;
	return (answer);
}

/*****************************************************************/
// direction: 0 == client to server. 1 == server to client.

int craftIpv4(char *payload, int payloadSize, char direction, struct handover *ho)
{
	unsigned int minimalLength;
	size_t packetLen = sizeof(struct ip) + payloadSize;
	unsigned char minimalip[(minimalLength = 12 + payloadSize)];
	char ipv4packet[packetLen];
	struct ip *iphdr = (struct ip *)ipv4packet;

	memset(ipv4packet, 0, packetLen);
	memset(minimalip, 0, minimalLength);

	if (payload != NULL)
		memcpy(((unsigned char *)(ipv4packet)) + sizeof(struct ip), payload, payloadSize);

	if (direction == TO_SERVER)
	{
		*((unsigned long *)((unsigned char *)minimalip + 0)) = ho->srcIP;
		*((unsigned long *)((unsigned char *)minimalip + 4)) = ho->dstIP;
	}
	else
	{
		*((unsigned long *)((unsigned char *)minimalip + 0)) = ho->dstIP;
		*((unsigned long *)((unsigned char *)minimalip + 4)) = ho->srcIP;
	}

	*((unsigned char *)((unsigned char *)minimalip + 8)) = 0;
	*((unsigned char *)((unsigned char *)minimalip + 9)) = IPPROTO_TCP;
	*((unsigned short *)((unsigned char *)minimalip + 10)) = htons(packetLen - sizeof(struct ip));

	iphdr->ip_v = 4;
	iphdr->ip_hl = 5;

	iphdr->ip_id = rand() & 0xFFFF;

	if (direction == TO_SERVER)
	{
		iphdr->ip_src.s_addr = ho->srcIP;
		iphdr->ip_dst.s_addr = ho->dstIP;
	}
	else
	{
		iphdr->ip_src.s_addr = ho->dstIP;
		iphdr->ip_dst.s_addr = ho->srcIP;
	}
	iphdr->ip_p = IPPROTO_TCP;
	iphdr->ip_ttl = 64;
	iphdr->ip_len = ntohs(packetLen);
	iphdr->ip_sum = 0;
	iphdr->ip_sum = ipChecksum((u_short *)iphdr, sizeof(struct ip));

	memcpy(minimalip + 12, ((unsigned char *)ipv4packet) + sizeof(struct ip), packetLen - sizeof(struct ip));

	write(fileno(outFile), ipv4packet, packetLen);

	return 0;
}

/*****************************************************************/
// direction: 0 == client to server. 1 == server to client.

int craftIpv6(char *payload, int payloadSize, char direction, struct handover *ho)
{
	size_t packetLen = sizeof(struct ip6_hdr) + payloadSize;
	char ipv6packet[packetLen];
	uint16_t tcp6_checksum(struct ip6_hdr, struct tcphdr);
	struct ip6_hdr *iphdr = (struct ip6_hdr *)ipv6packet;

	memset(ipv6packet, 0, sizeof(ipv6packet));

	if (payload != NULL)
		memcpy(((unsigned char *)(ipv6packet)) + sizeof(struct ip6_hdr), payload, payloadSize);

	iphdr->ip6_ctlun.ip6_un1.ip6_un1_hlim = 64;
	iphdr->ip6_ctlun.ip6_un1.ip6_un1_flow = htonl((6 << 28) | (0 << 20) | 0);
	iphdr->ip6_ctlun.ip6_un1.ip6_un1_plen = htons(packetLen - sizeof(struct ip6_hdr));
	iphdr->ip6_ctlun.ip6_un1.ip6_un1_nxt = IPPROTO_TCP;

	if (direction == TO_SERVER)
	{
		memcpy(&(iphdr->ip6_src), &ho->srcIP6, sizeof(iphdr->ip6_src));
		memcpy(&(iphdr->ip6_dst), &ho->dstIP6, sizeof(iphdr->ip6_dst));
	}
	else
	{
		memcpy(&(iphdr->ip6_src), &ho->dstIP6, sizeof(iphdr->ip6_src));
		memcpy(&(iphdr->ip6_dst), &ho->srcIP6, sizeof(iphdr->ip6_dst));
	}

	write(fileno(outFile), ipv6packet, packetLen);

	return 0;
}

/*****************************************************************/
// direction: 0 == client to server. 1 == server to client.

int craftTcp(char *payload, int payloadSize, char direction, unsigned char flags, struct handover *ho)
{
	size_t packetLen;
	char packet[(packetLen = sizeof(struct tcphdr) + payloadSize)];
	struct tcphdr *tcpheader = (struct tcphdr *)((unsigned char *)packet);
	unsigned int minimalLength, minimalLength6;
	unsigned char minimalip[(minimalLength = 12 + sizeof(struct tcphdr) + payloadSize)];
	unsigned char minimalip6[(minimalLength6 = 40 + packetLen)];
	char ipv4packet[sizeof(struct ip) + packetLen];
	struct v6_pseudo_header *v6PseudoHeader = (struct v6_pseudo_header *)minimalip6;

	memset(packet, 0, packetLen);
	memset(ipv4packet, 0, sizeof(ipv4packet));

	if (payload != NULL)
		memcpy(((unsigned char *)(packet)) + sizeof(struct tcphdr), payload, payloadSize);

	if (ho->ipV == 4)
	{
		if (direction == TO_SERVER)
		{
			*((unsigned long *)((unsigned char *)minimalip + 0)) = ho->srcIP;
			*((unsigned long *)((unsigned char *)minimalip + 4)) = ho->dstIP;
		}
		else
		{
			*((unsigned long *)((unsigned char *)minimalip + 0)) = ho->dstIP;
			*((unsigned long *)((unsigned char *)minimalip + 4)) = ho->srcIP;
		}

		*((unsigned char *)((unsigned char *)minimalip + 8)) = 0;
		*((unsigned char *)((unsigned char *)minimalip + 9)) = IPPROTO_TCP;
		*((unsigned short *)((unsigned char *)minimalip + 10)) = htons(packetLen);
	}
	else
	{
		memset(v6PseudoHeader, 0, sizeof(*v6PseudoHeader));

		if (direction == TO_SERVER)
		{
			memcpy(&(v6PseudoHeader->src), &ho->srcIP6, sizeof(v6PseudoHeader->src));
			memcpy(&(v6PseudoHeader->dst), &ho->dstIP6, sizeof(v6PseudoHeader->dst));
		}
		else
		{
			memcpy(&(v6PseudoHeader->src), &ho->dstIP6, sizeof(v6PseudoHeader->src));
			memcpy(&(v6PseudoHeader->dst), &ho->srcIP6, sizeof(v6PseudoHeader->dst));
		}

		v6PseudoHeader->length = htonl(packetLen);
		v6PseudoHeader->next_header = IPPROTO_TCP;

		tcpheader->th_sum = 0;
	}

	if (direction == TO_SERVER)
	{
		tcpheader->th_sport = htons(ho->srcPort);
		tcpheader->th_dport = htons(ho->dstPort);
	}
	else
	{
		tcpheader->th_sport = htons(ho->dstPort);
		tcpheader->th_dport = htons(ho->srcPort);
	}

	if (direction == TO_SERVER)
	{
		tcpheader->th_seq = htonl(ho->seq);
		tcpheader->th_ack = htonl(ho->ack_seq);
	}
	else
	{
		tcpheader->th_seq = htonl(ho->ack_seq);
		tcpheader->th_ack = htonl(ho->seq);
	}

	tcpheader->th_off = ((sizeof(struct tcphdr)) / 4);

	tcpheader->th_flags = flags;

	tcpheader->th_win = htons(5840);

	if (ho->ipV == 4)
	{
		memcpy(minimalip + 12, ((unsigned char *)packet), packetLen);
		tcpheader->th_sum = ipChecksum((u_short *)&minimalip, minimalLength);
		craftIpv4(packet, packetLen, direction, ho);
	}
	else
	{
		memcpy(minimalip6 + 40, ((unsigned char *)packet), packetLen);
		tcpheader->th_sum = ipChecksum((u_short *)&minimalip6, minimalLength6);
		craftIpv6(packet, packetLen, direction, ho);
	}

	if (direction == TO_SERVER)
		ho->seq = ho->seq + payloadSize;
	else
		ho->ack_seq += payloadSize;

	return 0;
}

/**************************************************************************************************************/

int tcpSendString(struct handover *ho, char *string, char direction)
{
	int packetLen = -1, direction2 = -1;

	if (direction == TO_SERVER)
	{
		direction2 = FROM_SERVER;
	}
	else
	{
		direction2 = TO_SERVER;
	}

	if (ho->ipV == 4)
		packetLen = packetLen4 + strlen(string);
	else
		packetLen = packetLen6 + strlen(string);

	ph.time = ho->time;
	ph.usec = ho->usec;

	ph.usec += INTERVAL;

	if ((ph.usec + INTERVAL) >= 1000000)
	{
		ph.time += 1;
		ph.usec = 0;
	}

	ph.length1 = packetLen;
	ph.length2 = packetLen;

	write(fileno(ho->outFile), &ph, sizeof(struct pcap_packet_header));

	if (direction == TO_SERVER)
		write(fileno(ho->outFile), ho->toEther, etherLen);
	else
		write(fileno(ho->outFile), ho->fromEther, etherLen);

	craftTcp(string, strlen(string), direction, TH_ACK | TH_PUSH, ho);

	//and now send the ack
	ph.usec += INTERVAL;
	ph.length1 = packetLen - strlen(string);
	ph.length2 = packetLen - strlen(string);
	write(fileno(ho->outFile), &ph, sizeof(struct pcap_packet_header));

	if (direction == TO_SERVER)
		write(fileno(ho->outFile), ho->fromEther, etherLen);
	else
		write(fileno(ho->outFile), ho->toEther, etherLen);

	craftTcp(NULL, 0, direction2, TH_ACK, ho);

	ho->time = ph.time;
	ho->usec = ph.usec;

	return strlen(string);
}

/**********************************************************************************************/

int tcpHandshake(struct handover *ho)
{
	int packetLen;

	if (ho->ipV == 4)
		packetLen = packetLen4;
	else
		packetLen = packetLen6;

	ph.usec += INTERVAL;
	ph.length1 = packetLen;
	ph.length2 = packetLen;

	ho->seq = rand() & 0xfff;
	ho->seq += 1;
	ho->ack_seq = 0;

	//client to server SYN
	write(fileno(outFile), &ph, sizeof(struct pcap_packet_header));

	//replace that with a sprintf and a single write

	write(fileno(outFile), ho->toEther, etherLen);

	craftTcp(NULL, 0, TO_SERVER, TH_SYN, ho); //direction 0 - client to server

	ho->ack_seq = rand() & 0xfff;
	ho->ack_seq += 1;
	ho->seq += 1;

	//and now send the SYN/ACK
	ph.usec += INTERVAL;
	write(fileno(outFile), &ph, sizeof(struct pcap_packet_header));
	write(fileno(outFile), ho->fromEther, etherLen);
	craftTcp(NULL, 0, FROM_SERVER, TH_SYN | TH_ACK, ho); //direction 1 - server to client

	ho->ack_seq += 1;

	//client to server ACK
	ph.usec += INTERVAL;
	write(fileno(outFile), &ph, sizeof(struct pcap_packet_header));
	write(fileno(outFile), ho->toEther, etherLen);
	craftTcp(NULL, 0, TO_SERVER, TH_ACK, ho); //direction 0 - client to server

	ho->time = ph.time;
	ho->usec = ph.usec;

	return 0;
}

/*******************************************************************************************/

int tcpShutdown(struct handover *ho)
{
	int packetLen;

	if (ho->ipV == 4)
		packetLen = packetLen4;
	else
		packetLen = packetLen6;

	ph.time = ho->time;
	ph.usec = ho->usec;

	ph.usec += INTERVAL;
	ph.length1 = packetLen;
	ph.length2 = packetLen;

	//server to client FIN
	write(fileno(outFile), &ph, sizeof(struct pcap_packet_header));
	write(fileno(outFile), ho->fromEther, etherLen);
	craftTcp(NULL, 0, FROM_SERVER, TH_ACK | TH_FIN, ho); //direction - server to client

	ho->ack_seq += 1;

	//and now send the ack
	ph.usec += INTERVAL;
	write(fileno(outFile), &ph, sizeof(struct pcap_packet_header));
	write(fileno(outFile), ho->toEther, etherLen);

	craftTcp(NULL, 0, TO_SERVER, TH_ACK, ho); //direction - client to server

	//client to server FIN
	ph.usec += INTERVAL;
	write(fileno(outFile), &ph, sizeof(struct pcap_packet_header));
	write(fileno(outFile), ho->toEther, etherLen);
	craftTcp(NULL, 0, TO_SERVER, TH_ACK | TH_FIN, ho); //direction - client to server

	ho->seq += 1;

	//and now send the ack
	ph.usec += INTERVAL;
	write(fileno(outFile), &ph, sizeof(struct pcap_packet_header));
	write(fileno(outFile), ho->fromEther, etherLen);
	craftTcp(NULL, 0, FROM_SERVER, TH_ACK, ho); //direction - server to client

	//	printf("Pcap replay length: %d second(s)\n", ph.time - 0x48f35358 + 1);

	return (0);
}

/****************************************************************************************************/

int openOutFile(struct handover *ho, char *inFileName, char *suffix)
{
	char buffer[500];

	if (strlen(ho->dstFile) == 0)
	{
		snprintf(buffer, sizeof(buffer) - 1, "%s%s", inFileName, suffix);
	}
	else
		snprintf(buffer, sizeof(buffer) - 1, "%s", ho->dstFile);

	if ((outFile = fopen(buffer, "w")) == NULL)
	{
		printf("Failed to open outfile %s\n", buffer);
		//                fclose(inFile);
		exit(-1);
	}
	else
		ho->outFile = outFile;

	printf("Writing to %s\n", buffer);

	write(fileno(outFile), &pcap_file_header, sizeof(pcap_file_header));

	return 0;
}

/*************************************************************************************************/

int ftp(struct handover *ho, char mode)
{

	srcport = rand() & 0x7fff; //0 <-> 32k
	srcport += 1025;
	hoFtp.srcPort = srcport;

	//initialize the time in the handover struct. The times have to be correct in all streams
	hoFtp.time = ho->time;
	hoFtp.usec = ho->usec;

	//snprintf(hoFtp.srcFile, sizeof(hoFtp.srcFile)-1, "%s", ho->srcFile);
	hoFtp.inFile = ho->inFile;
	hoFtp.outFile = ho->outFile;

	//fill all the parameters into the handover struct
	if (mode == PASSIVE_FTP)
	{
		hoFtp.srcIP = ho->srcIP;
		hoFtp.dstIP = ho->dstIP;

		memcpy(hoFtp.srcIP6, ho->srcIP6, sizeof(hoFtp.srcIP6));
		memcpy(hoFtp.dstIP6, ho->dstIP6, sizeof(hoFtp.dstIP6));

		memcpy(hoFtp.srcEther, DST_ETHER, sizeof(hoFtp.dstEther));
		memcpy(hoFtp.dstEther, SRC_ETHER, sizeof(hoFtp.srcEther));
	}
	else
	{
		hoFtp.srcPort = htons(20);
		hoFtp.srcIP = ho->dstIP;
		hoFtp.dstIP = ho->srcIP;

		memcpy(hoFtp.srcIP6, ho->dstIP6, sizeof(hoFtp.srcIP6));
		memcpy(hoFtp.dstIP6, ho->srcIP6, sizeof(hoFtp.dstIP6));

		memcpy(hoFtp.srcEther, DST_ETHER, sizeof(hoFtp.srcEther));
		memcpy(hoFtp.dstEther, SRC_ETHER, sizeof(hoFtp.dstEther));
	}

	//Set the protocol the Ethernet is carrying. IPv4 or IPv6
	if (hoFtp.ipV == 4)
		memcpy(hoFtp.protoEther, PROTO_ETHER, 2);
	else
		memcpy(hoFtp.protoEther, PROTO_ETHER6, 2);

	snprintf(hoFtp.srcFile, sizeof(hoFtp.srcFile) - 1, "%s", ho->srcFile);

	if (ho->dstFile != NULL)
		snprintf(hoFtp.dstFile, sizeof(hoFtp.dstFile) - 1, "%s", ho->dstFile);

	//	ho.outFile = NULL;
	hoFtp.dstPort = 20;

	if (mode == PASSIVE_FTP)
	{
		hoFtp.direction = FROM_SERVER;
		memcpy(hoFtp.toEther, hoFtp.dstEther, 6);
		memcpy(hoFtp.toEther + 6, hoFtp.srcEther, 6);
		memcpy(hoFtp.toEther + 12, hoFtp.protoEther, 2);

		memcpy(hoFtp.fromEther, hoFtp.srcEther, 6);
		memcpy(hoFtp.fromEther + 6, hoFtp.dstEther, 6);
		memcpy(hoFtp.fromEther + 12, hoFtp.protoEther, 2);
	}
	else
	{
		ho->direction = FROM_SERVER;
		hoFtp.direction = TO_SERVER;

		memcpy(hoFtp.toEther, hoFtp.srcEther, 6);
		memcpy(hoFtp.toEther + 6, hoFtp.dstEther, 6);
		memcpy(hoFtp.toEther + 12, hoFtp.protoEther, 2);

		memcpy(hoFtp.fromEther, hoFtp.dstEther, 6);
		memcpy(hoFtp.fromEther + 6, hoFtp.srcEther, 6);
		memcpy(hoFtp.fromEther + 12, hoFtp.protoEther, 2);
	}

	tcpHandshake(ho);

	if (mode == ACTIVE_FTP)
		hoFtp.dstPort = ftpCommandsStartActive(ho);
	else
		hoFtp.dstPort = ftpCommandsStartPassive(ho);

	tcpHandshake(&hoFtp);
	ftpTransferFile(&hoFtp);
	tcpShutdown(&hoFtp);

	ho->time = hoFtp.time;
	ho->usec = hoFtp.usec;

	ftpCommandsEnd(ho);
	tcpShutdown(ho);

	return 0;
}

/***********************************************************************************************/

int httpGet(struct handover *ho)
{

	tcpHandshake(ho);
	httpGetRequest(ho);
	httpGetRequestAcknowledge(ho);
	ho->direction = FROM_SERVER;
	httpTransferFile(ho);
	tcpShutdown(ho);

	return 0;
}

/***********************************************************************************************/

int http2Get(struct handover *ho)
{

	tcpHandshake(ho);
	http2ConnectionUpgrade(ho);
	http2SwitchingProtocols(ho);
	http2ClientMagic(ho);
	//	http2MagicGetRequest(ho);

	//send settings from server to client
	ho->direction = FROM_SERVER;
	http2Settings(ho);

	//ack server settings and send settings from client to server
	ho->direction = TO_SERVER;
	http2SettingsAck(ho);
	http2Settings(ho);

	//ack client settings
	ho->direction = FROM_SERVER;
	http2SettingsAck(ho);

	ho->direction = TO_SERVER;
	http2ClientGetRequest(ho);

	ho->direction = FROM_SERVER;
	http2Headers(ho);
	http2TransferFile(ho);
	http2DataStreamClose(ho);
	http2GoAway(ho);
	tcpShutdown(ho);

	return 0;
}

/***********************************************************************************************/

int httpPost(struct handover *ho)
{

	tcpHandshake(ho);
	httpPostRequest(ho);
	httpTransferFile(ho);
	httpPostFinalBoundary(ho);
    ho->direction = FROM_SERVER;
	httpPostRequestAcknowledge(ho);
	tcpShutdown(ho);

	return 0;
}

/*******************************************************************************************/

int smtp(struct handover *ho)
{

	tcpHandshake(ho);
	smtpRequest(ho);
	tcpShutdown(ho);

	return 0;
}

/*********************************************************************************************/

int pop3(struct handover *ho)
{

	tcpHandshake(ho);
	pop3Request(ho);
	tcpShutdown(ho);

	return 0;
}

/********************************************************************************************/

int imap(struct handover *ho)
{

	tcpHandshake(ho);
	imapRequest(ho);
	tcpShutdown(ho);

	return 0;
}

/********************************************************************************************/

int main(int argc, char **argv)
{
	char *modeString = NULL, *srcFile = NULL, *dstFile = NULL, *portString = NULL, *tok1 = NULL, *tok2 = NULL, *encoderString = NULL;
	int c, i;
	struct stat statbuf, statbuf2;
	struct handover ho;
    uint16_t vlan = 0;

	if (argc < 2)
		usage();

	//Default is IPv4
	ho.ipV = 4;
	hoFtp.ipV = 4;

	while ((c = getopt(argc, argv, "v:e:m:o:p:6")) != -1)
	{
		switch (c)
		{
        case 'v':
            vlan = (uint16_t)atoi(optarg);
            if (vlan == 0 || vlan > 4095) /* yes, I know, priority tagging... */
            {
                fprintf(stderr, "vlan must be between 1 and 4095\n");
                exit(1);
            }
            vlan = htons(0xfff & vlan);
            packetLen4 += 4;
            packetLen6 += 4;
            etherLen += 4;
            break;

		case 'e':
			encoderString = (char *)strdup(optarg);
			break;

			/*			case 'f':
				encoderString = (char*) strdup(optarg);
				break;
*/
		case 'm':
			modeString = (char *)strdup(optarg);
			break;

		case 'o':
			dstFile = (char *)strdup(optarg);
			break;

		case 'p':
			portString = (char *)strdup(optarg);
			break;

		case '6':
			ho.ipV = 6;
			hoFtp.ipV = 6;
			break;
			/*
			case 'v':
				test = (char*)strdup(optarg);
				if(test != NULL)
printf("POSITIVE! - #%s#\n", test);
				break;
*/
		default:
			usage();
			exit(0);
		}
	}

	if (optind < argc)
	{
		srcFile = (char *)strdup(argv[optind]);
	}

	if ((srcFile == NULL))
	{
		printf("Input file not found\n");
		exit(-1);
	}

	//check if the input file is a directory, quit if true
	if (stat(srcFile, &statbuf) == 0)
	{
		if (S_ISDIR(statbuf.st_mode))
		{
			printf("Input has to be a file, not a directory\n");
			exit(-1);
		}
	}
	else
	{
		printf("Input file %s not found\n", srcFile);
		exit(-1);
	}

	if ((inFile = fopen(srcFile, "r")) == NULL)
	{
		printf("Failed to open infile %s\n", srcFile);
		exit(-1);
	}
	else
	{
		ho.inFile = inFile;
		hoFtp.inFile = inFile;
	}

	if (fstat(fileno(inFile), &fileStat) == -1)
	{
		printf("Failed to read size of %s\n", srcFile);
		fclose(inFile);
		exit(-1);
	}

	//init the PRNG
	srand(getpid() * time(NULL));
	srcport = rand() & 0x7fff; //0 <-> 32k
	srcport += 1025;
	ho.srcPort = srcport;
	ph.time = 0x48f35358;
	ph.usec = 0;

	//initialize the time in the handover struct. The times have to be correct in all streams
	ho.time = ph.time;
	ho.usec = ph.usec;

	//fill all the parameters into the handover struct
	ho.srcIP = inet_addr(random_ipv4(0));
	ho.dstIP = inet_addr(random_ipv4(1));
	inet_pton(AF_INET6, SRC_IP6, &(ho.srcIP6));
	inet_pton(AF_INET6, DST_IP6, &(ho.dstIP6));
	memcpy(ho.srcEther, SRC_ETHER, sizeof(ho.srcEther));
	memcpy(ho.dstEther, DST_ETHER, sizeof(ho.dstEther));

	//Set the protocol the Ethernet is carrying. IPv4 or IPv6
	if (ho.ipV == 4)
		memcpy(ho.protoEther, PROTO_ETHER, 2);
	else
		memcpy(ho.protoEther, PROTO_ETHER6, 2);

	snprintf(ho.srcFile, sizeof(ho.srcFile) - 1, "%s", srcFile);

	memset(ho.dstFile, 0, sizeof(ho.dstFile));
	if (dstFile != NULL)
	{
		if (stat(dstFile, &statbuf2) == 0)
		{
			if ((statbuf.st_dev == statbuf2.st_dev) && (statbuf.st_ino == statbuf2.st_ino))
			{
				printf("Output file cannot be input file!\n");
				exit(-1);
			}
		}

		snprintf(ho.dstFile, sizeof(ho.dstFile) - 1, "%s", dstFile);
	}

	ho.outFile = NULL;
	ho.encoder = ENC_BASE64;
	ho.dstPort = 0;

	memcpy(ho.toEther, ho.srcEther, 6);
	memcpy(ho.toEther + 6, ho.dstEther, 6);
    if (vlan)
    {
        memcpy(ho.toEther + 12, PROTO_8021Q, 2);
        memcpy(ho.toEther + 14, &vlan, 2);
        memcpy(ho.toEther + 16, ho.protoEther, 2);
    } else
	    memcpy(ho.toEther + 12, ho.protoEther, 2);

	memcpy(ho.fromEther, ho.dstEther, 6);
	memcpy(ho.fromEther + 6, ho.srcEther, 6);
    if (vlan)
    {
        memcpy(ho.fromEther + 12, PROTO_8021Q, 2);
        memcpy(ho.fromEther + 14, &vlan, 2);
        memcpy(ho.fromEther + 16, ho.protoEther, 2);
    } else
	    memcpy(ho.fromEther + 12, ho.protoEther, 2);

	if (portString != NULL)
	{
		if (strstr(portString, ":") != NULL)
		{
			tok1 = strtok(portString, ":");
			ho.srcPort = atoi(tok1);

			if (tok1 != NULL)
				tok2 = strtok(NULL, ":");
			else
			{
				usage();
				exit(-1);
			}

			if (tok2 != NULL)
				ho.dstPort = atoi(tok2);
			else
			{
				usage();
				exit(-1);
			}
		}
		else
		{
			ho.dstPort = atoi(portString);
		}
	}

	if (encoderString != NULL)
	{
		switch (atoi(encoderString))
		{
		case 0:
			ho.encoder = ENC_BASE64;
			break;
		case 1:
			ho.encoder = ENC_QUOTED_PRINTABLE;
			break;

		case 2:
			ho.encoder = ENC_UU;
			break;
		}
	}

	if (modeString != NULL)
	{
		if (strlen(modeString) > 1)
		{
			if (ho.dstPort != 0)
				printf("Specifying a destination port is not possible when using multiple modes. Switching to default destination ports.\n");

			ho.dstPort = 0;

			if (dstFile != NULL)
			{
				printf("Specifying an output filename does not work when using multiple modes\n");
				usage();
				exit(-1);
			}
		}

		for (i = 0; i < strlen(modeString); i++)
		{
			switch (modeString[i])
			{
			case 'f':
                if (vlan)
                {
                    fprintf(stderr, "VLAN is not supported with FTP mode\n");
                    exit(1);
                }

				if (ho.dstPort == 0)
					ho.dstPort = 21;

				openOutFile(&ho, basename(srcFile), "-ftp-active.pcap");
				ftp(&ho, ACTIVE_FTP);

				fclose(ho.outFile);
				ho.dstPort = 0;
				break;

			case 'F':
                if (vlan)
                {
                    fprintf(stderr, "VLAN is not supported with FTP mode\n");
                    exit(1);
                }

				if (ho.dstPort == 0)
					ho.dstPort = 21;

				openOutFile(&ho, basename(srcFile), "-ftp-passive.pcap");
				ftp(&ho, PASSIVE_FTP);

				fclose(ho.outFile);
				ho.dstPort = 0;
				//					modeString[i] = '\0';
				break;

			case 'h':
				if (ho.dstPort == 0)
					ho.dstPort = 80;

				if (modeString[i + 1] == 0x00)
				{
					openOutFile(&ho, basename(srcFile), "-http-get.pcap");
					httpGet(&ho);
				}
				else if (modeString[i + 1] == '2')
				{
					openOutFile(&ho, basename(srcFile), "-http2-get.pcap");
					http2Get(&ho);
					i++;
				}
				fclose(ho.outFile);
				ho.dstPort = 0;
				break;

			case 'H':
				if (ho.dstPort == 0)
					ho.dstPort = 80;
				openOutFile(&ho, basename(srcFile), "-http-post.pcap");
				httpPost(&ho);
				fclose(ho.outFile);
				ho.dstPort = 0;
				break;

			case 's':
				if (ho.dstPort == 0)
					ho.dstPort = 25;
				openOutFile(&ho, basename(srcFile), "-smtp.pcap");
				smtp(&ho);
				fclose(ho.outFile);
				ho.dstPort = 0;
				break;

			case 'p':
				if (ho.dstPort == 0)
					ho.dstPort = 110;
				openOutFile(&ho, basename(srcFile), "-pop3.pcap");
				pop3(&ho);
				fclose(ho.outFile);
				ho.dstPort = 0;
				break;

			case 'i':
				if (ho.dstPort == 0)
					ho.dstPort = 143;
				openOutFile(&ho, basename(srcFile), "-imap.pcap");
				imap(&ho);
				fclose(ho.outFile);
				ho.dstPort = 0;
				break;

			case 'x':
				if (ho.dstPort == 0)
					ho.dstPort = 80;
				openOutFile(&ho, basename(srcFile), "-http2-get.pcap");

				http2Get(&ho);
				fclose(ho.outFile);
				ho.dstPort = 0;
				break;

			default:
				usage();
				exit(-1);
				break;
			}
		}
	}
	else
	{
		if (ho.dstPort == 0)
			ho.dstPort = 80;
		openOutFile(&ho, basename(srcFile), "-http-get.pcap");
		httpGet(&ho);
		fclose(ho.outFile);
	}

	exit(0);
}
