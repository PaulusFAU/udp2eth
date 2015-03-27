// udp2eth.cc
//
// Author:		Jan de Jongh
// Version:		0.2
// Organization:	TNO
//
// (C) 2010, 2011, 2015, Jan de Jongh
// (C) 2010, 2011, 2015, TNO
//
 
#include <sys/types.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <unistd.h>
#include <net/if.h>
#include <net/ethernet.h>
#include <netpacket/packet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <netdb.h>
#include <stdio.h>
#include <errno.h>
#include <arpa/inet.h>

#ifndef OPTIONMM_command_line
#include <optionmm/command_line.hh>
#endif

using namespace std;

//
// Version numbers.
//
static const int VersionMajor = 0;
static const int VersionMinor = 2;

//
// Defaults and boundaries.
//

static const std::string default_device_s  = "wlan0";
static const std::string default_server_s = "0.0.0.0:1235"; 
static const std::string default_client_s = "127.0.0.1:1236";

//
// Ethernet packet socket.
//
static int eth_sock = -1;

//
// UDP server socket.
// (Needed by callback functions below.)
//
static int udp_server_sock = -1;

//
// UDP client address.
//
static struct sockaddr_in *udp_client_address = NULL;

//
// Parse a URL and return host and port (in host byte order).
// Allowed forms:
// 	<host>:<port>
// 	<host>
// 	<host>:
// 	:<port>
// 	:
// 	(empty)
// where host is either a hostname or an IPv4 address (both in dotted notation).
//
// NOTE/CAVEAT: IPv4 ONLY!!
//
// Returns 0 upon success, -1 or exit upon failure.
//
static int parse_url
  (const std::string &url, const std::string &default_url, std::string &host, std::string &port,
   bool fail_is_fatal = true)
{
  // Dissect default URL first.
  std::string default_host;
  std::string default_port;
  if (default_url.find_first_of (':') != string::npos
    && default_url.find_first_of (':') == default_url.find_last_of (':'))
  {
    default_host = default_url.substr (0, default_url.find_first_of (':'));
    default_port = default_url.substr (default_url.find_first_of (':') + 1);
  }
  else
  {
    std::cerr << "[udp2eth] parse_url: improperly constructed default URL: "
              << default_url << '.' << std::endl;
    if (fail_is_fatal) exit (-1);
    return -1;
  }
  // Now deal with the URL.
  if (url.find_first_of (':') != url.find_last_of (':'))
  {
    std::cerr << "[udp2eth] parse_url: improperly constructed URL: "
              << url << '.' << std::endl;
    if (fail_is_fatal) exit (-1);
    return -1;
  }
  if (url.find_first_of (':') == string::npos)
  {
    if (not url.empty ())
    {
      host = url;
    }
    else
    {
      host = default_host;
    }
    port = default_port;
  }
  else
  {
    host = url.substr (0, url.find_first_of (':'));
    if (host.empty ())
    {
      host = default_host;
    }
    port = url.substr (url.find_first_of (':') + 1);
    if (port.empty ())
    {
      port = default_port;
    }
  }
  return 0;
}

//
// Parse a URL and return a new socket for it, optionally binding it.
//
static int create_socket
  (const std::string &url, const std::string &default_url, const int socktype = SOCK_DGRAM, const bool attempt_bind = true)
{
  std::string host;
  std::string port;
  parse_url (url, default_url, host, port);
  struct addrinfo ai_hints, *ai;
  memset (&ai_hints, 0, sizeof (ai_hints));
  ai_hints.ai_family = AF_INET;
  ai_hints.ai_socktype = socktype;
  int gai_ret = getaddrinfo (host.c_str (), port.c_str (), &ai_hints, &ai);
  if (gai_ret != 0)
  {
    cerr << "[udp2eth] create_socket, getaddrinfo returned error: " << gai_strerror (gai_ret) << "." << std::endl;
    return -1;
  }
  if (ai == NULL)
  {
    cerr << "[udp2eth] create_socket, getaddrinfo returned no addresses, unexpected!" << std::endl;
    return -1;
  }
  if (ai->ai_next != NULL)
  {
    cerr << "[udp2eth] create_socket, ambiguous host/port specification, host = " << host
         << ", port = " << port << "."
         << std::endl;
    return -1;
  }
  const int sock = socket (ai->ai_family, ai->ai_socktype, ai->ai_protocol);
  if (sock == -1)
  {
    cerr << "[udp2eth] create_socket, could not create socket for host = " << host
         << ", port = " << port << "."
         << std::endl;
    return -1;
  }
  if (attempt_bind)
  {
    if (bind (sock, ai->ai_addr, ai->ai_addrlen) == -1)
    {
      perror ("[udp2eth] create_socket, bind failed");
      close (sock);
      return -1;
    }
  }
  return sock;
}

//
// Open a UDP socket for given sockaddr_in.
//
static int open_udp_socket (const sockaddr_in *address)
{
  int sock;
  sock = socket (AF_INET, SOCK_DGRAM, 0);
  if (sock < 0)
  {
    std::cerr << "[udp2eth] open_udp_socket: unable to create socket." << std::endl;
    exit (-1);
  }
  if (bind (sock, (struct sockaddr *) address, sizeof (*address)) < 0)
  { 
    std::cerr << "[udp2eth] open_udp_socket: unable to bind socket." << std::endl;
    exit (-1);
  }
  return sock;
}

//
// Open a UDP socket on specified port (in host-byte order), listening on INADDR_ANY.
//
static int open_udp_socket (const uint16_t port)
{
  int sock, length;
  struct sockaddr_in server;
  sock = socket (AF_INET, SOCK_DGRAM, 0);
  if (sock < 0)
  {
    std::cerr << "[udp2eth] open_udp_socket: unable to create socket." << std::endl;
    exit (-1);
  }
  length = sizeof (server);
  bzero (&server, length);
  server.sin_family = AF_INET;
  server.sin_addr.s_addr = INADDR_ANY;
  server.sin_port = htons (port);
  if (bind (sock, (struct sockaddr *) &server, length) < 0)
  { 
    std::cerr << "[udp2eth] open_udp_socket: unable to bind socket." << std::endl;
    exit (-1);
  }
  return sock;
}

//
// Open a AF_PACKET socket with user-supplied Ethernet header (SOCK_RAW) for given interface name.
//

static int open_eth_socket (const std::string &eth_name, const bool promiscuous)
{
  // Create the socket.
  int sock;
  sock = socket (AF_PACKET, SOCK_RAW, htons (ETH_P_ALL));
  if (sock < 0)
  {
    std::cerr << "[udp2eth] open_eth_socket: unable to create socket." << std::endl;
    exit (-1);
  }
  // Find the Ethernet interface index.
  struct ifreq ifr;
  memset (&ifr, 0, sizeof (ifr));
  const size_t eth_name_len = strlen (eth_name.c_str ());
  if (eth_name_len < sizeof (ifr.ifr_name))
  {
    memcpy (ifr.ifr_name, eth_name.c_str (), eth_name_len);
    ifr.ifr_name[eth_name_len] = 0;
  }
  else
  {
    std::cerr << "[udp2eth] open_eth_socket: interface name " << eth_name << " too long." << std::endl;
    close (sock);
    exit (-1);
  }
  if (ioctl (sock, SIOCGIFINDEX, &ifr) == -1)
  {
    std::cerr << "[udp2eth] open_eth_socket: Ethernet interface " << eth_name << " not found." << std::endl;
    close (sock);
    exit (-1);
  }
  // The following piece of code apparently does not work for the socket type.
  //if (setsockopt (sock, SOL_SOCKET, SO_BINDTODEVICE, (void *) &ifr, sizeof (ifr)) < 0)
  //{
  //  std::cerr << "[udp2eth] open_eth_socket: unable to bind socket to interface with name "
  //            << eth_name << "." << std::endl;
  //  close (sock);
  //  exit  (-1);
  //}
  struct sockaddr_ll sll;
  memset (&sll, 0, sizeof (sll));
  sll.sll_family = AF_PACKET; 
  sll.sll_ifindex = ifr.ifr_ifindex; 
  sll.sll_protocol = htons (ETH_P_ALL); 
  if (bind (sock, (struct sockaddr *) &sll , sizeof (sll)) == -1)
  {
    std::cerr << "[udp2eth] open_eth_socket: unable to bind socket to interface with name "
              << eth_name << "." << std::endl;
    close (sock);
    exit (-1);
  }
  if (promiscuous)
  {
    /* Get the current flags that the device might have. */
    if (ioctl (sock, SIOCGIFFLAGS, &ifr) == -1)
    {
      std::cerr << "[udp2eth] open_eth_socket: unable to get device flags for "
                << eth_name << "." << std::endl;
      close (sock);
      exit (-1);
    }
    else
    {
      /* Set the old flags plus the IFF_PROMISC flag. */
      ifr.ifr_flags |= IFF_PROMISC;
      if (ioctl (sock, SIOCSIFFLAGS, &ifr) == -1)
      {
        std::cerr << "[udp2eth] open_eth_socket: could not enter "
                  << eth_name << "into promiscuous mode." << std::endl;
        close (sock);
        exit (-1);
      }
      printf ("Entering promiscuous mode.\n");
    }
  }
  return sock;
}

//
// Main entry.
//
int main (int argc, char** argv)
{

  //
  // Gather command-line options.
  // To see the effect of this, just do a <prog-name> -h, or a <prog-name> -l.
  //
  stringstream version;
  version << VersionMajor << "." << VersionMinor;
  optionmm::basic_command_line<> cl ("udp2eth", version.str (), "Copyright (C) 2011-2015 TNO/GCDC/I-GAME", "", argc, argv);
  optionmm::bool_option list_opt ('l', "list-parameters-only", "List parameters on stderr and exit", false);
  cl.add (list_opt);
  optionmm::bool_option verbose_opt ('v', "verbose", "Verbose debugging", false);
  cl.add (verbose_opt);
  optionmm::basic_option<std::string,true,false> device_opt ('\0', "device", "(Ethernet) Device name", default_device_s);
  cl.add (device_opt);
  optionmm::bool_option promiscuous_opt ('p', "promiscuous", "Attempt to open device in promiscuous mode", false);
  cl.add (promiscuous_opt);
  optionmm::basic_option<std::string,true,false> server_opt ('\0', "server", "Server address", default_server_s);
  cl.add (server_opt);
  optionmm::basic_option<std::string,true,false> client_opt ('\0', "client", "Client address", default_client_s);
  cl.add (client_opt);

  //
  // Collect the options.
  //
  if (not cl.process ())
  {
    std::cerr << "[udp2eth] Error processing command-line arguments, aborting!" << std::endl;
    return -1;
  }

  //
  // Do not accept unprocessed arguments; probably typos!
  //
  if (argc > 1)
  {
    std::cerr << "[udp2eth] " << argc - 1 << " unprocessed command-line arguments [try 'udp2eth --help']:" << std::endl;
    for (int i = 1; i < argc; i++)
    {
      std::cerr << "  " << i << ": " << argv[i] << std::endl;
    }
    std::cerr << "[udp2eth] Aborting!" << std::endl;
    return -1;
  }

  //
  // Return immediately if user requested help or version.
  //
  if (cl.help ())
  {
    // User requested help only.
    return 0;
  }
  if (cl.version ())
  {
    // User requested version only.
    return 0;
  }

  //
  // Collect parameters from options.
  //
  const bool list_parameters_only = list_opt.value (0);
  const bool verbose = verbose_opt.value (0);
  const std::string device_s = device_opt.value (0);
  const bool promiscuous = promiscuous_opt.value (0);
  const std::string server_s = server_opt.value (0);
  const std::string client_s = client_opt.value (0);

  //
  // Report command-line options on std::cerr.
  //
  std::cerr << "[udp2eth] Device name   : " << device_s << std::endl;
  std::cerr << "[udp2eth]   Promiscuous : " << (promiscuous ? "Yes" :  "No") << std::endl;
  std::cerr << "[udp2eth] Server address: " << server_s << std::endl;
  std::cerr << "[udp2eth] Client address: " << client_s << std::endl;

  //
  // Exit if we're requested to only list parameters.
  //
  if (list_parameters_only)
  {
    std::cerr << std::endl << "[udp2eth] Listing parameters only, exiting!" << std::endl;
    return (0);
  }

  //
  // Ethernet socket.
  //

  // Nothing to do here...

  //
  // UDP client addess.
  //
  std::string client_host;
  std::string client_port;
  if (parse_url (client_s.c_str (), default_client_s.c_str (), client_host, client_port, false) == 0)
  {
    udp_client_address = new (struct sockaddr_in);
    if (udp_client_address == NULL)
    {
      std::cerr << "[udp2eth] main, out of memory!" << std::endl;
    }
    else
    {
      memset (udp_client_address, 0, sizeof (*udp_client_address));
      udp_client_address->sin_family = AF_INET;
      udp_client_address->sin_port = htons (atoi (client_port.c_str ()));
      inet_pton (AF_INET, client_host.c_str (), &udp_client_address->sin_addr);
    }
  }

  while (true)
  {
    //
    // Open the Ethernet socket, if needed.
    //
    if (eth_sock < 0)
    {
      eth_sock = open_eth_socket (device_s, promiscuous);
    }
    //
    // Open the UDP server socket for incoming payload.
    //
    if (udp_server_sock < 0)
    {
      udp_server_sock = create_socket (server_s.c_str (), default_server_s.c_str ());
    }
    //
    // Prepare an fd_set for use with select () later on.
    // We'll wait for input from eth_sock and from udp_server_sock.
    // Note that we must rebuild this in the main loop, because select () modifies it.
    //
    int highest_sock = -1;
    fd_set isocks;
    FD_ZERO (&isocks);
    if (eth_sock >= 0)
    {
      FD_SET (eth_sock, &isocks);
      if (eth_sock > highest_sock)
      {
        highest_sock = eth_sock;
      }
    }
    if (udp_server_sock >= 0)
    {
      FD_SET (udp_server_sock, &isocks);
      if (udp_server_sock > highest_sock)
      {
        highest_sock = udp_server_sock;
      }
    }
    //
    // Timeout for select ().
    //
    struct timeval timeout;
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
    //
    // Select () and process result.
    //
    const int select_ret = select
      (highest_sock + 1, (highest_sock >= 0) ? &isocks : (fd_set *) 0, (fd_set *) 0, (fd_set *) 0, &timeout);
    if (select_ret < 0)
    {
      perror ("[udp2eth] Select returned negative value");
      exit (-1);
    }
    else if (select_ret == 0)
    {
      // Timeout?
    }
    else
    {
      if (eth_sock >= 0 && FD_ISSET (eth_sock, &isocks))
      {
        if (verbose)
        {
          std::cerr << "[udp2eth] eth_sock." << std::endl;
        }
        // Read from eth_sock until exhaustion.
        bool need_more = true;
        while (eth_sock >= 0 && need_more)
        {
          unsigned char buf[4096];
          const int recv_ret = recv (eth_sock, buf, 4096, MSG_DONTWAIT);
          if (recv_ret > 0)
          {
            if (verbose)
            {
              std::cerr << "[udp2eth] eth_sock: received " << recv_ret << " bytes!" << std::endl;
            }
            // Send to udp_server_sock...
            if (udp_server_sock >= 0)
            {
              // if (send (udp_server_sock, buf, recv_ret, 0) == -1)
              if (sendto (udp_server_sock, buf, recv_ret, 0, (struct sockaddr *) udp_client_address,
                          sizeof (*udp_client_address)) == -1)
              {
                std::cerr << "[udp2eth] Failed to send to udp_server_sock!" << std::endl;
              }
            }
            else
            {
              std::cerr << "[udp2eth] No udp_server_sock!" << std::endl;
            }
          }
          else if (recv_ret == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
          {
            need_more = false;
          }
          else if (recv_ret == 0)
          {
            std::cerr << "[udp2eth] Recv from Ethernet socket return 0, indicating shutdown. This should not happen!" << std::endl;
            close (eth_sock);
            eth_sock = -1;
          }
          else
          {
            std::cerr << "[udp2eth] Recv from Ethernet socket return unexpected negative value!" << std::endl;
            close (eth_sock);
            eth_sock = -1;
          }
        }
      }
      if (udp_server_sock >= 0 && FD_ISSET (udp_server_sock, &isocks))
      {
        if (verbose)
        {
          std::cerr << "[udp2eth] udp_server_sock." << std::endl;
        }
        // Read from udp_server_sock until exhaustion.
        bool need_more = true;
        while (udp_server_sock >= 0 && need_more)
        {
          unsigned char buf[4096];
          const int recv_ret = recv (udp_server_sock, buf, 4096, MSG_DONTWAIT);
          if (recv_ret > 0)
          {
            if (verbose)
            {
              std::cerr << "[udp2eth] udp_server_sock: received " << recv_ret << " bytes!" << std::endl;
            }
            // Send to eth_sock...
            if (eth_sock >= 0)
            {
              if (send (eth_sock, buf, recv_ret, 0) == -1)
              // if (sendto (eth_sock, buf, recv_ret, 0, (struct sockaddr *) udp_client_address,
              //            sizeof (*udp_client_address)) == -1)
              {
                std::cerr << "[udp2eth] Failed to send to eth_sock!" << std::endl;
              }
            }
            else
            {
              std::cerr << "[udp2eth] No eth_sock!" << std::endl;
            }
          }
          else if (recv_ret == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
          {
            need_more = false;
          }
          else if (recv_ret == 0)
          {
            std::cerr << "[udp2eth] Recv from UDP socket return 0, indicating shutdown. This should not happen!" << std::endl;
            close (udp_server_sock);
            udp_server_sock = -1;
          }
          else
          {
            std::cerr << "[udp2eth] Recv from UDP socket return unexpected negative value!" << std::endl;
            close (udp_server_sock);
            udp_server_sock = -1;
          }
        }
      }
    }
  }

  // Never reached.
  // Return "All OK" value.
  return 0;

}
