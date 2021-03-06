/* -*-  Mode: C++; c-file-style: "gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2011 University of California, Los Angeles
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Ilya Moiseenko <iliamo@cs.ucla.edu>
 *         Alexander Afanasyev <alexander.afanasyev@ucla.edu>
 */
 /**
  * Modified by Tang, <tangjianqiang@bjtu.edu.cn>
  * National Engineering Lab for Next Generation Internet Interconnection Devices,
  * School of Electronics and Information Engineering,
  * Beijing Jiaotong Univeristy, Beijing 100044, China.
**/

#include "ndn-producer.h"
#include "ns3/log.h"
#include "ns3/ndn-interest-header.h"
#include "ns3/ndn-content-object-header.h"
#include "ns3/string.h"
#include "ns3/uinteger.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"

#include "ns3/ndn-app-face.h"
#include "ns3/ndn-fib.h"
// #include "../model/ndn-fib-impl.h"

#include <boost/ref.hpp>
#include <boost/lambda/lambda.hpp>
#include <boost/lambda/bind.hpp>
namespace ll = boost::lambda;

NS_LOG_COMPONENT_DEFINE ("ndn.Producer");

namespace ns3 {
namespace ndn {

NS_OBJECT_ENSURE_REGISTERED (Producer);
    
TypeId
Producer::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::ndn::Producer")
    .SetGroupName ("Ndn")
    .SetParent<App> ()
    .AddConstructor<Producer> ()
    .AddAttribute ("Prefix","Prefix, for which producer has the data",
                   StringValue ("/"),
                   MakeNameComponentsAccessor (&Producer::m_prefix),
                   MakeNameComponentsChecker ())
    .AddAttribute ("Locator","Locator, for which locator the mobile producer has attached",
                   StringValue ("/"),
                   MakeNameComponentsAccessor (&Producer::m_locatorName),
                   MakeNameComponentsChecker ())
    .AddAttribute ("PayloadSize", "Virtual payload size for Content packets",
                   UintegerValue (1024),
                   MakeUintegerAccessor(&Producer::m_virtualPayloadSize),
                   MakeUintegerChecker<uint32_t>())

    // optional attributes
    .AddAttribute ("SignatureBits", "SignatureBits field",
                   UintegerValue (0),
                   MakeUintegerAccessor(&Producer::m_signatureBits),
                   MakeUintegerChecker<uint32_t> ())
    ;
        
  return tid;
}
    
Producer::Producer ()
	:m_positionPoint (-1)
{
  // NS_LOG_FUNCTION_NOARGS ();
}

// inherited from Application base class.
void
Producer::StartApplication ()
{
  NS_LOG_FUNCTION_NOARGS ();
  NS_ASSERT (GetNode ()->GetObject<Fib> () != 0);

  App::StartApplication ();

  NS_LOG_DEBUG ("NodeID: " << GetNode ()->GetId ());
  
  Ptr<Fib> fib = GetNode ()->GetObject<Fib> ();
  
  Ptr<fib::Entry> fibEntry = fib->Add (m_prefix, m_face, 0);

  fibEntry->UpdateStatus (m_face, fib::FaceMetric::NDN_FIB_GREEN);
  
  // // make face green, so it will be used primarily
  // StaticCast<fib::FibImpl> (fib)->modify (fibEntry,
  //                                        ll::bind (&fib::Entry::UpdateStatus,
  //                                                  ll::_1, m_face, fib::FaceMetric::NDN_FIB_GREEN));
}

void
Producer::StopApplication ()
{
  NS_LOG_FUNCTION_NOARGS ();
  NS_ASSERT (GetNode ()->GetObject<Fib> () != 0);

  App::StopApplication ();
}


void
Producer::OnInterest (const Ptr<const InterestHeader> &interest, Ptr<Packet> origPacket)
{
  App::OnInterest (interest, origPacket); // tracing inside

  NS_LOG_FUNCTION (this << interest);

  if (!m_active) return;

  //we can do something here, check the next node, or forward to the new locator.
  //if(interest->GetLocator()==m_prefix) return;
  //if(!(interest->GetName().cut(1) == m_prefix)) return;
  
  static ContentObjectTail tail;
  Ptr<ContentObjectHeader> header = Create<ContentObjectHeader> ();
  header->SetName (Create<NameComponents> (interest->GetName ()));
  if (interest->IsEnabledLocator()&&(interest->GetLocator().size ()>0))
  {
    if(m_locatorName.size()>0)
    {
	header->SetLocator (Create<NameComponents> (m_locatorName));
    }
  }
  if (interest->IsEnabledLocator()&&(interest->GetLocator().size ()>0))
  {
      m_positionPoint=1;
      header->SetPosition (m_positionPoint);
  }
  else
  {
      m_positionPoint=-1;
      header->SetPosition (m_positionPoint);
  }
  header->GetSignedInfo ().SetTimestamp (Simulator::Now ());
  header->GetSignature ().SetSignatureBits (m_signatureBits);

  NS_LOG_INFO ("node("<< GetNode()->GetId() <<") respodning with ContentObject:\n" << boost::cref(*header));
  
  Ptr<Packet> packet = Create<Packet> (m_virtualPayloadSize);
  // Ptr<const WeightsPathStretchTag> tag = origPacket->RemovePacketTag<WeightsPathStretchTag> ();
  // if (tag != 0)
  //   {
  //     // std::cout << Simulator::Now () << ", " << m_app->GetInstanceTypeId ().GetName () << "\n";

  //     // echo back WeightsPathStretchTag
  //     packet->AddPacketTag (CreateObject<WeightsPathStretchTag> (*tag));

  //     // \todo
  //     // packet->AddPacketTag should actually accept Ptr<const WeightsPathStretchTag> instead of
  //     // Ptr<WeightsPathStretchTag>.  Echoing will be simplified after change is done
  //   }
  
  packet->AddHeader (*header);
  packet->AddTrailer (tail);

  m_protocolHandler (packet);
  
  m_transmittedContentObjects (header, packet, this, m_face);
}

} // namespace ndn
} // namespace ns3
