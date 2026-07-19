/*
 * uav-net-sim.cc — UAV co-simulation with a Wi-Fi jamming attacker and a
 * reactive channel-hopping defense.
 *
 * Based on FlyNetSim (Baidya, Shaikh, Levorato, ACM MSWiM 2018),
 * https://github.com/saburhb/FlyNetSim — the two-simulator core and the MyApp
 * traffic class are reused from that project. The channel-hopping defense
 * (CheckJamming / DoChannelHop / ReadThreshold), the TCP stream reassembly, and
 * the per-lane scheduling are additions in this work.
 *
 * Links ns-3 (GPLv2); distributed under the GNU GPL v2.
 * See LICENSE and NOTICE at the repository root.
 */
#include "ns3/flow-monitor-module.h"
#include "ns3/wifi-net-device.h"
#include <iostream>
#include <fstream>
#include <string>
#include <map>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector> 
#include <time.h>
#include <ctime>
#include <czmq.h>
#undef LOG_DEBUG
#undef LOG_INFO
#include <libxml/parser.h>
#include <libxml/xmlIO.h>
#include <libxml/xinclude.h>
#include <libxml/tree.h>

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/mobility-module.h"
#include "ns3/wifi-module.h"
#include "ns3/internet-module.h"
#include "ns3/applications-module.h"
#include "ns3/ipv4-global-routing-helper.h"
#include "ns3/lte-helper.h"
#include "ns3/epc-helper.h"
#include "ns3/lte-module.h"
#include "ns3/point-to-point-module.h"
#include "myApps.h"
#include "myInput.h"


/************ GLOBAL CONSTANTS ************/
long g_cmd_start = 0;
long g_cmd_end = 0;
long g_cmd_tot_delay = 0;
long g_cmd_num = 0;


pthread_t tid_gcs[2];
pthread_t tid_uav[2];

// Channel-hopping defense state
uint32_t g_sendCount = 0;
uint32_t g_recvCount = 0;




bool g_jammingDetected = false;
double g_threshold = 70.0;  // default; overwritten by ReadThreshold() from config.xml
NetDeviceContainer g_devicesWifiSta;
NetDeviceContainer g_devicesWifiAp;


// Prepare our context and publisher for Commands to UAVs from GCS
void *contextCm = zmq_ctx_new (); 
void *publisherCm = zmq_socket (contextCm, ZMQ_PUB);

// Prepare our context and publisher for Telemetry from UAVs
void *contextTm = zmq_ctx_new (); 
void *publisherTm = zmq_socket (contextTm, ZMQ_PUB);

// Prepare our context and publisher for video sensor data
void *contextVid = zmq_ctx_new (); 
void *publisherVid = zmq_socket (contextVid, ZMQ_PUB);


//global subscriber for use in the rcvpacket callback
int rc;
void *context = zmq_ctx_new ();
void *subscriber = zmq_socket (context, ZMQ_SUB);



using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("uav-net-sim");

vector <Ptr<MyApp>> appVectCom;
vector <Ptr<MyApp>> appVectTel;


// Process one complete "@@@...***" frame (or one raw congestion packet).
// RcvPacket() reassembles the TCP byte stream and calls this once per complete frame.
void ProcessFrame(const char *frameIn, size_t frameLen)
{
        char *buffer = (char *) malloc(frameLen + 50);
        memset(buffer, 0, frameLen + 50);
        memcpy(buffer, frameIn, frameLen);
        char *string = buffer;
        char *sTime = (char *)malloc(20);
        memset(sTime, '\0', 20);
        sprintf(sTime, "%10ld***", ns3::Simulator::Now().GetMilliSeconds());
        strcat(string, sTime);

	size_t sLen = strlen(string) + 50; char *s = (char *) malloc(sLen);
        memset(s, '\0', sLen);
	strcpy(s, string);
  
        if(string[3] == 'G' || string[3] == 'U') g_recvCount++;
        if(string[3] == 'G')   // Packet from GCS to UAV with Control Commands
        {
          zmq_msg_t message;
          zmq_msg_init_size (&message, strlen (string));
          memcpy (zmq_msg_data (&message), string, strlen (string));
          printf(" Publish MSG : %s \n", string);
          zmq_sendmsg (publisherCm, &message, 0);
          zmq_msg_close (&message);

	  // Tokenize to parse packet to get packet ingress and egress time : do it in every one sec interval	 
	  g_cmd_end = ns3::Simulator::Now().GetMilliSeconds();
	  g_cmd_num++;
	  
          char* token = strtok(s, "***");
	  int count = 0;
          long ingress_time = 0;
          long egress_time = 0;

	  while(token)
	  {
	      if(count == 4)
              {
             	ingress_time = (atol)(token);
              }
              else if(count == 5)
              {
               	egress_time = (atol)(token);
              }
              count++;
              token = strtok(NULL, "***");
	  }

	  long net_delay = egress_time - ingress_time + 1;
	  g_cmd_tot_delay += net_delay;

	  if((g_cmd_end - g_cmd_start) > 1000)   // print average network packet delay for command packets, in every sec
	  {
	      float g_cmd_avg_delay = (float)(g_cmd_tot_delay)/g_cmd_num;
              printf(">>>>>> Average Network Delay for command packets: %f MilliSec.\n", g_cmd_avg_delay);

	      //reset variables
	      g_cmd_tot_delay = 0;
	      g_cmd_num = 0; 
	      g_cmd_start = g_cmd_end;
	   }

        }
        else if (string[3] == 'U')  // Telemetry Packets from UAV to GCS
        {
         
          zmq_msg_t message;
          zmq_msg_init_size (&message, strlen (string));
          memcpy (zmq_msg_data (&message), string, strlen (string));
          printf(" Publish MSG : %s \n", string);
          zmq_sendmsg (publisherTm, &message, 0); //Telemetry Information
          std::cout << "Send DATA to EDGE >>>>>>>> " << std::endl;
          zmq_msg_close (&message);

        }
        else if(string[3] == 'S')   // for SENSOR data
        {
          printf("RECEIVED SENSOR DATA FROM NS3\n");

          while(true)
          {
            zmq_msg_t msg;
            zmq_msg_init (&msg);
            rc = (zmq_msg_recv (&msg, subscriber, 0));
            int size = zmq_msg_size (&msg);
            printf("ZMQ SENSOR DATA SIZE: %d \n", size);
            char *strVZ = (char*)malloc (size + 1);
            memcpy (strVZ, zmq_msg_data (&msg), size);
            strVZ[size] = '\0';
            printf("POPPED MSG from ZMQ: %s \n", strVZ);

            if(strncmp(string+9,strVZ+9, 6) > 0)
            {
            }
            else
            {
              printf(" Publish MSG from ZMQ: %s \n", strVZ);
              zmq_sendmsg (publisherTm, &msg, 0); //Telemetry Information
              break;
            } 
            zmq_msg_close (&msg);
            free(strVZ);
          }

        }
	else
	{

	}

        free(sTime);
        free(s);
        free(buffer);
}


// TCP is a byte stream: a "@@@<type>_000***<base64>***" frame can be split across
// segments or several glued into one. Buffer per-sender, emit only complete frames
// (cut on "@@@"), keep any trailing partial for next time. Congestion traffic never
// starts with '@' -> bypasses reassembly.
void RcvPacket(Ptr<const Packet> p, const Address &addr)
{
        uint8_t *buf8 = new uint8_t[p->GetSize() + 50];
        memset(buf8, 0, p->GetSize() + 50);
        p->CopyData(buf8, p->GetSize());
        char *seg = (char *)buf8;

        // Per-sender reassembly buffer (key = sender Address as string)
        static std::map<std::string, std::string> g_rxBuf;
        std::ostringstream keyss; keyss << addr;
        std::string key = keyss.str();
        std::map<std::string, std::string>::iterator it = g_rxBuf.find(key);
        bool framed = (seg[0] == '@') || (it != g_rxBuf.end() && !it->second.empty());
        if (framed) {
          std::string &rb = g_rxBuf[key];
          rb.append(seg, p->GetSize());
          size_t start = rb.find("@@@");
          if (start != std::string::npos) {
            size_t next;
            while ((next = rb.find("@@@", start + 3)) != std::string::npos) {
              ProcessFrame(rb.data() + start, next - start);  // one COMPLETE frame
              start = next;
            }
            rb.erase(0, start);   // keep trailing (possibly incomplete) frame for next segment
          }
        } else {
          ProcessFrame(seg, p->GetSize());   // unframed / congestion traffic
        }
        delete[] buf8;
}


void RcvPktTrace(){
        Config::ConnectWithoutContext("/NodeList/*/ApplicationList/*/$ns3::PacketSink/Rx", MakeCallback(&RcvPacket));
}

/*****************************************************/
/**** START rcvCommands(): commands from GCS-ZMQ  ****/ 
/**** send command to specific UAV node over socket***/
/*****************************************************/

void* rcvCommands(void *arg)
{

  std::cout << "STRUCT VOID : " << arg << std::endl; 
  vector <Ptr<MyApp>> *vectCom = (vector <Ptr<MyApp>> *) arg;
  int numUav = vectCom->size();
  std::cout << "Number of UAV APPs receiving Commands: " << numUav << std::endl; 
  int rc;
  void *context = zmq_ctx_new ();
  void *subscriber = zmq_socket (context, ZMQ_SUB);  

  rc = zmq_connect (subscriber, "tcp://localhost:5500");
  assert (rc == 0);
  zmq_setsockopt( subscriber, ZMQ_SUBSCRIBE, "", 0);

  while(1)
  {
    zmq_msg_t message;
    zmq_msg_init (&message);
    rc = (zmq_msg_recv (&message, subscriber, 0));
    int size = zmq_msg_size (&message);
    char *string = (char*)malloc (size + 1 + 15);
    memcpy (string, zmq_msg_data (&message), size);
    zmq_msg_close (&message);
    string [size] = '\0';
    char sTime[15] = {"\0"};
    sprintf(sTime, "%11ld***", ns3::Simulator::Now().GetMilliSeconds());
    strcat(string, sTime);

    char uav_index[4]; // expects index starts from 0 from the external APP
    memcpy(uav_index, &string[5], 3);
    uav_index[3] = '\0';
    int sock_index = atoi(uav_index);
    Ptr<MyApp> app = (*vectCom)[sock_index]; //Pick specific UAV App 
 
    Ptr<Socket> send_socket = app->m_socket;
    pthread_mutex_lock(&tNext_mutex);
    // Commands use their own counter anchored to the current sim clock, so they don't
    // inherit the telemetry counter's growing delay. 1ms spacing between commands.
    static double cmd_schedule_time = 0.0;
    double cmdNowS = ns3::Simulator::Now().GetSeconds();
    if (cmd_schedule_time < cmdNowS) cmd_schedule_time = cmdNowS; // never lag behind the clock
    cmd_schedule_time += 0.001;                                   // 1ms spacing between commands
    Time tNext (Seconds (cmd_schedule_time - cmdNowS));           // tiny delay measured from "now"
    pthread_mutex_unlock(&tNext_mutex);
    ns3::Simulator::ScheduleWithContext(0xffffffff, tNext, &MyApp::SendMsg, app, send_socket, string);
  }

  zmq_close (subscriber);
  zmq_term (context);
  return 0;
}
/************** END rcvCommands() ***************/


/*****************************************************/
/**** START rcvTelemetry(): commands from UAV-ZMQ ****/ 
/****   send telemetry to  GCS node over socket    ***/
/*****************************************************/

void* rcvTelemetry(void *arg)
{
  zmq_bind (publisherVid, "tcp://127.0.0.1:5000");

  std::cout << "STRUCT VOID : " << arg << std::endl; 
  vector <Ptr<MyApp>> *vectTel = (vector <Ptr<MyApp>> *) arg;
  int numUav = vectTel->size();
  std::cout << "Number of UAV APPs sending telemetry: " << numUav << std::endl;

  int rc;
  void *context = zmq_ctx_new ();
  void *subscriber = zmq_socket (context, ZMQ_SUB);  

  rc = zmq_connect (subscriber, "tcp://localhost:5600");
  assert (rc == 0);
  zmq_setsockopt( subscriber, ZMQ_SUBSCRIBE, "", 0);

  while(1)
  {
    zmq_msg_t message;
    zmq_msg_init (&message);
    rc = (zmq_msg_recv (&message, subscriber, 0));
    int size = zmq_msg_size (&message);
    char *string = (char*)malloc (size + 1 + 23);
    memcpy (string, zmq_msg_data (&message), size);
    string [size] = '\0';

    printf(" TELEMETRY Recvd: Size %ld \n", strlen(string));
    printf(" TELEMETRY Recvd: %s \n", string);

    //check if SENSOR data received, then publish in video ZMQ
    char *prefix = (char*)malloc(7);
    memcpy (prefix, string, 6);
    prefix[6] = '\0';

    if(!strcmp(prefix, "SENSOR"))
    {
        zmq_sendmsg (publisherVid, &message, 0);
        zmq_msg_close(&message);
    }
    free(prefix);

    //Concatenate time at the end for telemetry
    char *sTime = (char *) malloc(20);
    memset(sTime, '\0', 20);
    sprintf(sTime, "%11ld***", ns3::Simulator::Now().GetMilliSeconds());
    strcat(string, sTime);

    //check the UAV ID that sent the telemetry
    char uav_index[4];
    memcpy(uav_index, &string[5], 3); 
    uav_index[3] = '\0';  //saved the UAV index


    /********** Now Update the location of UAV from the telemetry *****/
    /******** Intercept telemetry to get location info ********/
    char *loc_pattern = (char *) malloc(12);
    memcpy(loc_pattern, string+11, 11);
    loc_pattern[11] = '\0';

    std::cout << " *********** LOC PATTERN :::::: " << loc_pattern << " ***********" << std::endl;

    if(!strncmp(loc_pattern, "DISTANCE***", 11))  //If the message has the word 'DISTANCE', update the location of UAV
    { 
	//location update
        std::cout << "DISTANCE VALUE found " << std::endl;
        
        char *curX = (char *)malloc(15);
        char *curY = (char *)malloc(15);

        int size = 0;
        for( char *c = &string[22]; *c!='*'; c++){
            curX[size++] = *c;
        }
        curX[size] = '\0';
        size = size + 3; //increment to cover the '***'

        int size2 = 0;
        for( char *c = &string[22+size]; *c!='*'; c++){
            curY[size2++] = *c;
        }
        curY[size2] = '\0';

        printf(">>>>>> X-Value:  %s  ;  Y-Value:  %s <<<<<<\n", curX, curY);

        float cur_x = atof(curX);
        float cur_y = atof(curY);

        //Update mobility based on current position
        printf("X \t %f \t Y \t %f\n", cur_x, cur_y);
        Ptr<Node> uavNode = ns3::NodeList::GetNode(0); // Need to change for multi-UAV
        Ptr<ConstantPositionMobilityModel> mmUAV = uavNode->GetObject<ConstantPositionMobilityModel>();
        ns3::Simulator::ScheduleWithContext(0xffffffff, Seconds(0), &ConstantPositionMobilityModel::SetPosition, mmUAV, Vector(cur_x, cur_y, 10));

        free(curX);
        free(curY);
     
    }
    int is_not_distance = strncmp(loc_pattern, "DISTANCE***", 11);
    free(loc_pattern);
    // position update done for uav_index

    if(is_not_distance)  // The UAV message is a normal Telemetry and not a location update
    {
      /******** Send the telemetry to the GCS TCP App ********/
      int sock_index = atoi(uav_index);
      Ptr<MyApp> app = (*vectTel)[sock_index];  
      Ptr<Socket> send_socket = app->m_socket;
      std::cout << "SELECTED APP ------- " << app << std::endl;

      pthread_mutex_lock(&tNext_mutex);
      // Telemetry uses its own counter anchored to the current sim clock, so PARAM_VALUE
      // replies (and all telemetry) fire ~now instead of inheriting a growing delay.
      // 1ms spacing per packet keeps bursts spread out.
      static double telem_schedule_time = 0.0;
      double telemNowS = ns3::Simulator::Now().GetSeconds();
      if (telem_schedule_time < telemNowS) telem_schedule_time = telemNowS; // never lag behind clock
      telem_schedule_time += 0.001;                                         // 1ms spacing per packet
      Time tNext (Seconds (telem_schedule_time - telemNowS));               // tiny delay from "now"
      pthread_mutex_unlock(&tNext_mutex);
      ns3::Simulator::ScheduleWithContext(0xffffffff, tNext, &MyApp::SendMsg, app, send_socket, string);
    }
    zmq_msg_close (&message);

    free(sTime);
  }

  zmq_close (subscriber);
  zmq_term (context);
  return 0;
}
void DoChannelHop() {
    Ptr<WifiNetDevice> droneDevice = DynamicCast<WifiNetDevice>(g_devicesWifiSta.Get(0));
    droneDevice->GetPhy()->SetChannelNumber(6);
    Ptr<WifiNetDevice> apDevice = DynamicCast<WifiNetDevice>(g_devicesWifiAp.Get(0));
    apDevice->GetPhy()->SetChannelNumber(6);
    std::cout << "[CHANNEL HOP] Drone and AP now on Channel 6 at t="
              << Simulator::Now().GetSeconds() << "s" << std::endl;
}
void CheckJamming() {
    static bool firstWindow = true;

    if (firstWindow) {
        firstWindow = false;
        g_sendCount = 0;
        g_recvCount = 0;
        std::cout << "[JAMMING CHECK] t=" << Simulator::Now().GetSeconds()
                  << "s | Warmup window (t=0-10s) ignored, first detection at t=15s" << std::endl;
        Simulator::Schedule(Seconds(5.0), &CheckJamming);
        return;
    }

    if (g_sendCount > 0) {
        uint32_t lost = (g_sendCount > g_recvCount) ? (g_sendCount - g_recvCount) : 0;
        float lossPercent = (float)lost / g_sendCount * 100.0;
        std::string label = g_jammingDetected ? "[CHANNEL 6 STATUS]" : "[JAMMING CHECK]";
        std::cout << label << " t=" << Simulator::Now().GetSeconds()
                  << "s | Sent=" << std::dec << g_sendCount << " Recv=" << g_recvCount
                  << " Loss=" << lossPercent << "%"
                  << std::endl;
        if (lossPercent > g_threshold && !g_jammingDetected) {
            g_jammingDetected = true;
            std::cout << "[JAMMING DETECTED] Switching to Channel 6 at t="
                      << Simulator::Now().GetSeconds() << "s" << std::endl;
            Simulator::Schedule(Seconds(1.0), &DoChannelHop);
        }
    }
    g_sendCount = 0;
    g_recvCount = 0;
    Simulator::Schedule(Seconds(5.0), &CheckJamming);  // recurring every 5s
}
/************* MAIN() FUNCTION ***************/

double ReadThreshold ()
{
  double threshold = 70.0;
  std::ifstream file ("config.xml");
  if (!file.is_open ()) return threshold;
  std::string content ((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
  auto start = content.find ("<threshold>");
  auto end   = content.find ("</threshold>");
  if (start != std::string::npos && end != std::string::npos)
    threshold = std::stod (content.substr (start + 11, end - start - 11));
  return threshold;
}

int main (int argc, char *argv[])
{
  int nUAV = 1;
  int network_type = 0;
  int nCong = 10;
  float inputCongRate = 0.0;
  int congPktSize = 100;
  std::string congRate = "2Kbps"; // This rate will not have any effect, actual rate is taken from inputCongRate

  GlobalValue::Bind ("SimulatorImplementationType", StringValue ("ns3::RealtimeSimulatorImpl"));
  Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(1448));

  /*********** Bind Publisher socket ***********/
  zmq_bind (publisherCm, "tcp://127.0.0.1:5601");
  zmq_bind (publisherTm, "tcp://127.0.0.1:5501");


  /*********** Load input from XML file ***********/
  MyInput *input = new MyInput();
  input->loadInput();
  nUAV = input->m_num_uav;
  network_type = input->m_network;
  nCong = input->m_num_traffic;
  inputCongRate = input->m_traf_rate;
  congPktSize = input->m_traf_size;
  g_threshold = ReadThreshold ();

  std::cout << " INPUT FROM XML : " << std::endl;  
  std::cout << " ----------------------- : " << std::endl;  
  std::cout << " NUMBER OF UAV(s) : " << nUAV << std::endl;  
  std::cout << " NUMBER OF CONTENDING NODES : " << nCong << std::endl;  
  std::cout << " TRAFFIC RATE OF EACH CONTENDING NODES : " << inputCongRate << std::endl;  
  std::cout << " PACKET SIZE OF EACH OF CONTENDING NODES : " << congPktSize << std::endl;  

  if(network_type == 0)
    std::cout << "----------- CREATE WIFI NETWORK ------------" << std::endl;
  else
    std::cout << "----------- CREATE LTE NETWORK ------------" << std::endl;


  /***************** Define the Network Physical Layer *******************/

  NS_LOG_INFO ("Create UAV nodes.");
  NodeContainer uavNode;
  uavNode.Create (nUAV); // Number of UAVs read from the config.xml file
  NodeContainer congNode;
  congNode.Create (nCong); // Number of contending traffic nodes read from xml file

  /********* Set Position and Mobility for WiFi Sta nodes *******/
  // For all the UAVs
  Ptr<ListPositionAllocator> positionAllocUav = CreateObject<ListPositionAllocator> ();
  for (uint32_t i = 0; i < uavNode.GetN(); ++i){
    float xVal = input->m_x_values[i];
    float yVal = input->m_y_values[i];
    float zVal = input->m_z_values[i];
    positionAllocUav->Add (Vector (xVal, yVal, zVal));
  }
  MobilityHelper mobilityUav;
  mobilityUav.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
  mobilityUav.SetPositionAllocator(positionAllocUav);
  mobilityUav.Install (uavNode);

  // For all the external traffic nodes
  MobilityHelper mobilityCong;
  mobilityCong.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
  mobilityCong.SetPositionAllocator("ns3::UniformDiscPositionAllocator",
					"rho", DoubleValue(10),
					"X", DoubleValue(25.0),
					"Y", DoubleValue(25.0));
  mobilityCong.Install (congNode);


  /*********** Install IP stack on all WiFi Station nodes *********/ 
  InternetStackHelper internetUav;
  internetUav.Install (uavNode);
  internetUav.Install (congNode);


  /********************* Configure Default LTE Parameters ****************************/
  Config::SetDefault ("ns3::LteSpectrumPhy::CtrlErrorModelEnabled", BooleanValue (false));
  Config::SetDefault ("ns3::LteSpectrumPhy::DataErrorModelEnabled", BooleanValue (true));
  Config::SetDefault ("ns3::PfFfMacScheduler::HarqEnabled", BooleanValue (false));
  Config::SetDefault ("ns3::PfFfMacScheduler::CqiTimerThreshold", UintegerValue (10));
  Config::SetDefault ("ns3::LteEnbRrc::EpsBearerToRlcMapping",EnumValue(LteEnbRrc::RLC_AM_ALWAYS));
  Config::SetDefault ("ns3::LteEnbNetDevice::UlBandwidth", UintegerValue(100));
  Config::SetDefault ("ns3::LteEnbNetDevice::DlBandwidth", UintegerValue(100));
  Config::SetDefault ("ns3::LteUePhy::EnableUplinkPowerControl", BooleanValue (false));

  Ptr<LteHelper> lteHelper = CreateObject<LteHelper> ();
  Ptr<PointToPointEpcHelper>  epcHelper = CreateObject<PointToPointEpcHelper> ();
  lteHelper->SetEpcHelper (epcHelper);
  NS_LOG_INFO("Created the LTE Helper");

  //This creates the sgw/pgw node
  Ptr<Node> pgw = epcHelper->GetPgwNode ();

  // Create a single RemoteHost
  NodeContainer remoteHostContainer;
  remoteHostContainer.Create (1);
  Ptr<Node> remoteHost = remoteHostContainer.Get (0);  // This remote host will be in GCS
  InternetStackHelper internet;
  internet.Install (remoteHostContainer);

  /*********************** INTERNET stack in EPC *********************************/
  PointToPointHelper p2ph;
  p2ph.SetDeviceAttribute ("DataRate", DataRateValue (DataRate ("100Gb/s")));
  p2ph.SetDeviceAttribute ("Mtu", UintegerValue (1500));
  p2ph.SetChannelAttribute ("Delay", TimeValue (Seconds (0.00001)));
  NetDeviceContainer internetDevices = p2ph.Install (pgw, remoteHost);
  Ipv4AddressHelper ipv4h;
  ipv4h.SetBase ("1.0.0.0", "255.0.0.0");
  Ipv4InterfaceContainer internetIpIfaces = ipv4h.Assign (internetDevices);
  Ipv4Address remoteHostAddr = internetIpIfaces.GetAddress (1);

  Ipv4StaticRoutingHelper ipv4RoutingHelper;
  Ptr<Ipv4StaticRouting> remoteHostStaticRouting = ipv4RoutingHelper.GetStaticRouting (remoteHost->GetObject<Ipv4> ());
  remoteHostStaticRouting->AddNetworkRouteTo (Ipv4Address ("7.0.0.0"), Ipv4Mask ("255.0.0.0"), 1);

  // Explicitly create the nodes required by the topology
  NodeContainer enbNodes;
  enbNodes.Create(1);

  Ptr<ListPositionAllocator> positionAllocEnb = CreateObject<ListPositionAllocator> ();
  positionAllocEnb->Add (Vector (25.0, 25.0, 30.0));

  MobilityHelper mobilityLte;
  mobilityLte.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
  mobilityLte.SetPositionAllocator(positionAllocEnb);
  mobilityLte.Install (enbNodes);


  /**************** Scheduler, Propagation and Fading *********************/
  lteHelper->SetHandoverAlgorithmType ("ns3::NoOpHandoverAlgorithm"); // disable automatic handover
  lteHelper->SetAttribute ("PathlossModel", StringValue ("ns3::FriisPropagationLossModel"));

  /*************** Create Devices **************************/
  NetDeviceContainer enbDevices;
  enbDevices = lteHelper->InstallEnbDevice(enbNodes);

  NetDeviceContainer ueDevices;
  for (uint32_t u = 0; u < uavNode.GetN (); ++u)
  {
    ueDevices.Add(lteHelper->InstallUeDevice (uavNode.Get(u)));
  }

  
  NetDeviceContainer congueDevices;
  for (uint32_t u = 0; u < congNode.GetN (); ++u)
  {
    congueDevices.Add(lteHelper->InstallUeDevice (congNode.Get(u)));
  }


  /******************* INTERNET Stack in LTE ***********************/
  Ipv4InterfaceContainer ueIpIfaceList;
  // assign IP address to UEs
  for (uint32_t u = 0; u < uavNode.GetN (); ++u)
  {
    Ipv4AddressHelper ipv4;
    Ptr<Node> ue = uavNode.Get (u);
    Ptr<NetDevice> ueLteDevice = ueDevices.Get (u);
    Ipv4InterfaceContainer ueIpIface = epcHelper->AssignUeIpv4Address (NetDeviceContainer (ueLteDevice));
    ueIpIfaceList.Add(ueIpIface);

    // set the default gateway for the UE
    Ptr<Ipv4StaticRouting> ueStaticRouting = ipv4RoutingHelper.GetStaticRouting (ue->GetObject<Ipv4> ());
    ueStaticRouting->SetDefaultRoute (epcHelper->GetUeDefaultGatewayAddress (), 1);

    lteHelper->Attach(ueLteDevice, enbDevices.Get(0));
    lteHelper->ActivateDedicatedEpsBearer (ueLteDevice, EpsBearer (EpsBearer::NGBR_VIDEO_TCP_DEFAULT), EpcTft::Default ());
  }

  Ipv4InterfaceContainer congueIpIfaceList;
  // assign IP address to UEs
  for (uint32_t u = 0; u < congNode.GetN (); ++u)
  {
    Ipv4AddressHelper ipv4;
    Ptr<Node> congue = congNode.Get (u);
    Ptr<NetDevice> congueLteDevice = congueDevices.Get (u);
    Ipv4InterfaceContainer congueIpIface = epcHelper->AssignUeIpv4Address (NetDeviceContainer (congueLteDevice));
    congueIpIfaceList.Add(congueIpIface);

    // set the default gateway for the UE
    Ptr<Ipv4StaticRouting> congueStaticRouting = ipv4RoutingHelper.GetStaticRouting (congue->GetObject<Ipv4> ());
    congueStaticRouting->SetDefaultRoute (epcHelper->GetUeDefaultGatewayAddress (), 1);

    lteHelper->Attach(congueLteDevice, enbDevices.Get(0));
    lteHelper->ActivateDedicatedEpsBearer (congueLteDevice, EpsBearer (EpsBearer::NGBR_VIDEO_TCP_DEFAULT), EpcTft::Default ());
  }

  /********************** Finished LTE Networks ********************/



  /******************** Define WiFi stack *****************/
  NodeContainer nodesWifiAp;
  nodesWifiAp.Create (1);

  YansWifiPhyHelper wifiPhy = YansWifiPhyHelper::Default ();
  YansWifiChannelHelper wifiChannel = YansWifiChannelHelper::Default ();
  wifiPhy.SetChannel (wifiChannel.Create ());


  /*************** Create and configure MAC layer **************/
  Ssid ssid; 
  WifiHelper wifi;
  wifi.SetStandard(WIFI_PHY_STANDARD_80211g);
  WifiMacHelper wifiMac;
  wifi.SetRemoteStationManager ("ns3::ArfWifiManager");

  NetDeviceContainer devicesWifiAp;
  NetDeviceContainer devicesWifiSta;
  NetDeviceContainer devicesWifiCong;

  char ssidString[10] = {'\0'};
  sprintf(ssidString, "AP_1");
  ssid = Ssid (ssidString);

  wifiMac.SetType ("ns3::ApWifiMac",
                   "Ssid", SsidValue (ssid));
  devicesWifiAp.Add(wifi.Install (wifiPhy, wifiMac, nodesWifiAp.Get (0)));

  for (uint32_t i = 0; i < uavNode.GetN(); ++i){
    wifiMac.SetType ("ns3::StaWifiMac",
                   "Ssid", SsidValue (ssid),
                   "ActiveProbing", BooleanValue (false));
    devicesWifiSta.Add (wifi.Install (wifiPhy, wifiMac, NodeContainer (uavNode.Get (i)))); //for all UAVs
  }

  for (uint32_t k = 0; k < congNode.GetN(); ++k){
    wifiMac.SetType ("ns3::StaWifiMac",
                   "Ssid", SsidValue (ssid),
                   "ActiveProbing", BooleanValue (false));
    devicesWifiCong.Add (wifi.Install (wifiPhy, wifiMac, NodeContainer (congNode.Get (k))));
  }


 wifiPhy.EnablePcapAll("uav-flight");

  std::cout << " AP MAC ADDRESS : "  << devicesWifiAp.Get(0)->GetAddress() << std::endl;
  std::cout << " UAV MAC ADDRESS : "  << devicesWifiSta.Get(0)->GetAddress() << std::endl;  // Use this MAC address to track the SINR for specific UAV
  // Copy WiFi devices to globals for the channel-hopping defense
  g_devicesWifiSta = devicesWifiSta;
  g_devicesWifiAp = devicesWifiAp;


  /********** Define Initial Position and Mobility of WiFi AP ************/
  /***********************************************************************/
  // For the AP or base station
  Ptr<ListPositionAllocator> positionAllocAp = CreateObject<ListPositionAllocator> ();
  positionAllocAp->Add (Vector (25.0, 25.0, 20.0));
  MobilityHelper mobilityAp;
  mobilityAp.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
  mobilityAp.SetPositionAllocator(positionAllocAp);
  mobilityAp.Install (nodesWifiAp);

 
  /************** Define IP stack for WiFi AP *************/
  InternetStackHelper internetWifi;
  internetWifi.Install (nodesWifiAp);

  Ipv4AddressHelper ipv4Wifi;
  Ipv4InterfaceContainer interfacesWifiAp;
  Ipv4InterfaceContainer interfacesWifiSta;
  Ipv4InterfaceContainer interfacesWifiCong;
    char ipString[30] = {'\0'};
    sprintf(ipString, "10.10.1.0");
    ipv4Wifi.SetBase (ipString, "255.255.255.0");
    interfacesWifiAp.Add(ipv4Wifi.Assign (devicesWifiAp.Get(0)));
    for (uint32_t i = 0; i < uavNode.GetN(); ++i){
      interfacesWifiSta.Add(ipv4Wifi.Assign (devicesWifiSta.Get(i)));
    }
  interfacesWifiCong.Add(ipv4Wifi.Assign (devicesWifiCong));
   
  /******************************************************************/




  /************ Write Application *****************/
  /************************************************/


  if(network_type == 0)
  {
  /************** Uplink Data transfer for Telemetry *************/
  for (uint32_t i = 0; i < uavNode.GetN(); ++i){
    Ptr<Node> remoteWifiHost = nodesWifiAp.Get (0);
    Ipv4Address remoteWifiHostAddr = interfacesWifiAp.GetAddress (0);
    uint16_t sinkport = 100+i;
    Address sinkAddress(InetSocketAddress (remoteWifiHostAddr, sinkport));
    ApplicationContainer sinkApp;
    PacketSinkHelper packetSinkHelper("ns3::TcpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), sinkport));
    sinkApp = packetSinkHelper.Install(remoteWifiHost);
    sinkApp.Start(Seconds(0.0));

    Ptr<Socket> ns3TcpSocket = Socket::CreateSocket(uavNode.Get(i), TcpSocketFactory::GetTypeId());
    Ptr<MyApp> app = CreateObject<MyApp>();
    app->Setup(ns3TcpSocket, sinkAddress, 1400, 50000, DataRate("1Mbps"), (2*i), 1);
    uavNode.Get(i)->AddApplication(app);
    app->SetStartTime(Seconds(0.1));
    std::cout << "APP : " << app << std::endl;
    appVectTel.push_back(app);
  }
 }
 else
 {
  /************** LTE Uplink Data transfer for Telemetry *************/
  for (uint32_t i = 0; i < uavNode.GetN(); ++i){
    uint16_t sinkport = 110+i;
    Address sinkAddress(InetSocketAddress (remoteHostAddr, sinkport));
    ApplicationContainer sinkApp;
    PacketSinkHelper packetSinkHelper("ns3::TcpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), sinkport));
    sinkApp = packetSinkHelper.Install(remoteHost);
    sinkApp.Start(Seconds(0.1));

    Ptr<Socket> ns3TcpSocket = Socket::CreateSocket(uavNode.Get(i), TcpSocketFactory::GetTypeId());
    Ptr<MyApp> app = CreateObject<MyApp>();
    app->Setup(ns3TcpSocket, sinkAddress, 1400, 50000, DataRate("1Mbps"), (2*i), 1);
    uavNode.Get(i)->AddApplication(app);
    app->SetStartTime(Seconds(0.1));
    appVectTel.push_back(app);

  }

 }
 

 
  if(network_type == 1)
  {
  /************** LTE Downlink Data transfer for Control *************/
   for (uint32_t i = 0; i < uavNode.GetN(); ++i){
    Ptr<Node> remoteLteHost = uavNode.Get (i);
    Ipv4Address remoteLteHostAddr = ueIpIfaceList.GetAddress (i);

    uint16_t sinkport = 2000+i;
    Address sinkAddress(InetSocketAddress (remoteLteHostAddr, sinkport));
    ApplicationContainer sinkApp;
    PacketSinkHelper packetSinkHelper("ns3::TcpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), sinkport));
    sinkApp = packetSinkHelper.Install(remoteLteHost);
    sinkApp.Start(Seconds(0.0));

    Ptr<Socket> ns3TcpSocket = Socket::CreateSocket(remoteHost, TcpSocketFactory::GetTypeId());
    Ptr<MyApp> app = CreateObject<MyApp>();
    app->Setup(ns3TcpSocket, sinkAddress, 1400, 50000, DataRate("1Mbps"), (2*i), 1);
    remoteHost->AddApplication(app);
    app->SetStartTime(Seconds(0.1));
    std::cout << "Sending LTE packet on Downlink" << std::endl;
    appVectCom.push_back(app);
  }
 }
 else
 { 
  /************** Downlink Data transfer for Control commands *************/
  for (uint32_t i = 0; i < uavNode.GetN(); ++i){
    Ptr<Node> remoteWifiHost = uavNode.Get (i);
    Ipv4Address remoteWifiHostAddr = interfacesWifiSta.GetAddress (i);
    uint16_t sinkport = 1000+i;
    Address sinkAddress(InetSocketAddress (remoteWifiHostAddr, sinkport));
    ApplicationContainer sinkApp;
    PacketSinkHelper packetSinkHelper("ns3::TcpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), sinkport));
    sinkApp = packetSinkHelper.Install(remoteWifiHost);
    sinkApp.Start(Seconds(0.0));

    Ptr<Socket> ns3TcpSocket = Socket::CreateSocket(nodesWifiAp.Get(0), TcpSocketFactory::GetTypeId());
    Ptr<MyApp> app = CreateObject<MyApp>();
    app->Setup(ns3TcpSocket, sinkAddress, 1400, 50000, DataRate("1Mbps"), (2*i), 0);
    nodesWifiAp.Get(0)->AddApplication(app);
    app->SetStartTime(Seconds(0.1));
    std::cout << "APP : " << app << std::endl; 
    appVectCom.push_back(app);
  }
 }



 if(network_type == 0)
 {
   /************** WiFi Uplink Data transfer for Congestion *************/
  if (inputCongRate > 0.0)
  {
   for (uint32_t i = 0; i < congNode.GetN(); ++i){
    Ptr<Node> remoteHost = nodesWifiAp.Get (0);
    Ipv4Address remoteHostAddr = interfacesWifiAp.GetAddress (0);
    uint16_t sinkport = 1234+ i;
    Address sinkAddress(InetSocketAddress (remoteHostAddr, sinkport));
    ApplicationContainer sinkApp;
    PacketSinkHelper packetSinkHelper("ns3::TcpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), sinkport));
    sinkApp = packetSinkHelper.Install(remoteHost);
    sinkApp.Start(Seconds(0.1));

    Ptr<Socket> ns3TcpSocket = Socket::CreateSocket(congNode.Get(i), TcpSocketFactory::GetTypeId());
    Ptr<MyApp> app = CreateObject<MyApp>();
    app->Setup(ns3TcpSocket, sinkAddress, congPktSize, 500000000, DataRate(congRate), (101+i), 1);
    app->m_congId = i;
    app->m_rate = inputCongRate;
    congNode.Get(i)->AddApplication(app);
    app->SetStartTime(Seconds(0.1));
    std::cout << "Sending Congestion Traffic" <<std::endl;
    app->SendPacket();
   }
  }
  else
  {
    std::cout << " Congestion Traffic rate = 0" <<std::endl;
  }
 }
 else
 {
  if (inputCongRate > 0.0)
  {
   /************** LTE Uplink Data transfer for Congestion *************/
   for (uint32_t i = 0; i < congNode.GetN(); ++i){
    uint16_t sinkport = 500+i;
    Address sinkAddress(InetSocketAddress (remoteHostAddr, sinkport));
    ApplicationContainer sinkApp;
    PacketSinkHelper packetSinkHelper("ns3::TcpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), sinkport));
    sinkApp = packetSinkHelper.Install(remoteHost);
    sinkApp.Start(Seconds(0.1));

    Ptr<Socket> ns3TcpSocket = Socket::CreateSocket(congNode.Get(i), TcpSocketFactory::GetTypeId());
    Ptr<MyApp> app = CreateObject<MyApp>();
    app->Setup(ns3TcpSocket, sinkAddress, congPktSize, 500000000, DataRate(congRate), (101+i), 1);
    app->m_congId = i;
    app->m_rate = inputCongRate;
    congNode.Get(i)->AddApplication(app);
    app->SetStartTime(Seconds(0.1));
    std::cout << "Sending Congestion Traffic" <<std::endl;
    app->SendPacket();

   }
  }
  else
  {
    std::cout << " Congestion Traffic rate = 0" <<std::endl;
  }
 }
 

  /************* CREATE PUB SUB THREADS ******************/
  int err;
  err = pthread_create(&(tid_gcs[0]), NULL, &rcvCommands, (void *)(&appVectCom));
  if(err != 0)
            printf("\n can't create thread : [%s]", strerror(err));
    else
            printf("\n Command Thread created successfully \n");

  err = pthread_create(&(tid_uav[0]), NULL, &rcvTelemetry, (void *)(&appVectTel));
  if(err != 0)
            printf("\n can't create thread : [%s]", strerror(err));
    else
            printf("\n Telemetry Thread created successfully \n");


  sleep(2);
 
  rc = zmq_connect (subscriber, "tcp://localhost:5000");
  assert (rc == 0);
  zmq_setsockopt( subscriber, ZMQ_SUBSCRIBE, "", 0);

  /*****************************************************/


  std::cout << "Current Simulation Time :" << Simulator::Now().GetMilliSeconds () << std::endl;


  Simulator::Schedule(Seconds(1.001), RcvPktTrace);
  Simulator::Schedule(Seconds(10.0), &CheckJamming);  // first check at t=10s (warmup window)
  FlowMonitorHelper flowmon;
  Ptr<FlowMonitor> monitor = flowmon.InstallAll ();
  Simulator::Stop (Seconds (300.0));
  Simulator::Run ();
  monitor->SerializeToXmlFile ("flynetsim-results.xml", true, true);
  Simulator::Destroy ();
}
