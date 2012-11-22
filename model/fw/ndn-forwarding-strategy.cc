/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil -*- */
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
 * Author:  Alexander Afanasyev <alexander.afanasyev@ucla.edu>
 *          Ilya Moiseenko <iliamo@cs.ucla.edu>
 */

#include "ndn-forwarding-strategy.h"

#include "ns3/ndn-pit.h"
#include "ns3/ndn-pit-entry.h"
#include "ns3/ndn-interest-header.h"
#include "ns3/ndn-content-object-header.h"
#include "ns3/ndn-pit.h"
#include "ns3/ndn-fib.h"
#include "ns3/ndn-content-store.h"
#include "ns3/ndn-face.h"

#include "ns3/assert.h"
#include "ns3/ptr.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/boolean.h"
#include "ns3/string.h"

#include <boost/ref.hpp>
#include <boost/foreach.hpp>
#include <boost/lambda/lambda.hpp>
#include <boost/lambda/bind.hpp>
#include <boost/tuple/tuple.hpp>
namespace ll = boost::lambda;

NS_LOG_COMPONENT_DEFINE ("ndn.ForwardingStrategy");

namespace ns3 {
namespace ndn {

NS_OBJECT_ENSURE_REGISTERED (ForwardingStrategy);

TypeId ForwardingStrategy::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::ndn::ForwardingStrategy")
    .SetGroupName ("Ndn")
    .SetParent<Object> ()

    ////////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////////

    .AddTraceSource ("OutInterests",  "OutInterests",  MakeTraceSourceAccessor (&ForwardingStrategy::m_outInterests))
    .AddTraceSource ("InInterests",   "InInterests",   MakeTraceSourceAccessor (&ForwardingStrategy::m_inInterests))
    .AddTraceSource ("DropInterests", "DropInterests", MakeTraceSourceAccessor (&ForwardingStrategy::m_dropInterests))
    
    ////////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////////

    .AddTraceSource ("OutData",  "OutData",  MakeTraceSourceAccessor (&ForwardingStrategy::m_outData))
    .AddTraceSource ("InData",   "InData",   MakeTraceSourceAccessor (&ForwardingStrategy::m_inData))
    .AddTraceSource ("DropData", "DropData", MakeTraceSourceAccessor (&ForwardingStrategy::m_dropData))

    .AddAttribute ("CacheUnsolicitedData", "Cache overheard data that have not been requested",
                   BooleanValue (false),
                   MakeBooleanAccessor (&ForwardingStrategy::m_cacheUnsolicitedData),
                   MakeBooleanChecker ())

    .AddAttribute ("DetectRetransmissions", "If non-duplicate interest is received on the same face more than once, "
                                            "it is considered a retransmission",
                   BooleanValue (true),
                   MakeBooleanAccessor (&ForwardingStrategy::m_detectRetransmissions),
                   MakeBooleanChecker ())
    ;
  return tid;
}

ForwardingStrategy::ForwardingStrategy ()
{
}

ForwardingStrategy::~ForwardingStrategy ()
{
}

void
ForwardingStrategy::NotifyNewAggregate ()
{
  if (m_pit == 0)
    {
      m_pit = GetObject<Pit> ();
    }
  if (m_fib == 0)
    {
      m_fib = GetObject<Fib> ();
    }
  if (m_contentStore == 0)
    {
      m_contentStore = GetObject<ContentStore> ();
    }

  Object::NotifyNewAggregate ();
}

void
ForwardingStrategy::DoDispose ()
{
  m_pit = 0;
  m_contentStore = 0;
  m_fib = 0;

  Object::DoDispose ();
}

void
ForwardingStrategy::OnInterest (const Ptr<Face> &incomingFace,
                                    Ptr<InterestHeader> &header,
                                    const Ptr<const Packet> &packet)
{
  m_inInterests (header, incomingFace);

  //lookup the PIT Table, added by Tang
  Ptr<pit::Entry> pitEntry = m_pit->Lookup (*header);
  if (pitEntry == 0)
    {
    //create a new pit entry if there is no pit entry for the name, added by Tang
      pitEntry = m_pit->Create (header);
      if (pitEntry != 0)
        {
          DidCreatePitEntry (incomingFace, header, packet, pitEntry);
        }
      else
        {
          FailedToCreatePitEntry (incomingFace, header, packet);
          return;
        }
    }

  bool isDuplicated = true;
  //check whether have received the same interets.
  if (!pitEntry->IsNonceSeen (header->GetNonce ()))
    {
      pitEntry->AddSeenNonce (header->GetNonce ());
      isDuplicated = false;
    }

  //return for received the same interest
  //interest is identified by nonce, added by Tang
  if (isDuplicated) 
    {
      DidReceiveDuplicateInterest (incomingFace, header, packet, pitEntry);
      return;
    }

  Ptr<Packet> contentObject;
  Ptr<const ContentObjectHeader> contentObjectHeader; // used for tracing
  Ptr<const Packet> payload; // used for tracing

  //look up the content is the content store, added by Tang
  boost::tie (contentObject, contentObjectHeader, payload) = m_contentStore->Lookup (header);
  
  //find the content in content store and return data, added by Tang
  if (contentObject != 0)
    {
      NS_ASSERT (contentObjectHeader != 0);      

      pitEntry->AddIncoming (incomingFace/*, Seconds (1.0)*/);

      // Do data plane performance measurements
      WillSatisfyPendingInterest (0, pitEntry);

      // Actually satisfy pending interest
      SatisfyPendingInterest (0, contentObjectHeader, payload, contentObject, pitEntry);
      return;
    }

  //add face to the existing PIT entry, and return, added by Tang
  if (ShouldSuppressIncomingInterest (incomingFace, pitEntry))
    {
      pitEntry->AddIncoming (incomingFace/*, header->GetInterestLifetime ()*/);
      // update PIT entry lifetime
      pitEntry->UpdateLifetime (header->GetInterestLifetime ());

      // Suppress this interest if we're still expecting data from some other face
      NS_LOG_DEBUG ("Suppress interests");
      m_dropInterests (header, incomingFace);
      return;
    }

  //forward interest, added by Tang
  PropagateInterest (incomingFace, header, packet, pitEntry);
}

void
ForwardingStrategy::OnData (const Ptr<Face> &incomingFace,
                                Ptr<ContentObjectHeader> &header,
                                Ptr<Packet> &payload,
                                const Ptr<const Packet> &packet)
{
  NS_LOG_FUNCTION (incomingFace << header->GetName () << payload << packet);
  m_inData (header, payload, incomingFace);
  
  // Lookup PIT entry
  Ptr<pit::Entry> pitEntry = m_pit->Lookup (*header);
  if (pitEntry == 0)
    {
      DidReceiveUnsolicitedData (incomingFace, header, payload);
      return;
    }
  else
    {
      // Add or update entry in the content store
      m_contentStore->Add (header, payload);
    }

  while (pitEntry != 0)
    {
      // Do data plane performance measurements
      WillSatisfyPendingInterest (incomingFace, pitEntry);

      // Actually satisfy pending interest
      SatisfyPendingInterest (incomingFace, header, payload, packet, pitEntry);

      // Lookup another PIT entry
      pitEntry = m_pit->Lookup (*header);
    }
}


void
ForwardingStrategy::DidReceiveDuplicateInterest (const Ptr<Face> &incomingFace,
                                                     Ptr<InterestHeader> &header,
                                                     const Ptr<const Packet> &packet,
                                                     Ptr<pit::Entry> pitEntry)
{
  NS_LOG_FUNCTION (this << boost::cref (*incomingFace));
  /////////////////////////////////////////////////////////////////////////////////////////
  //                                                                                     //
  // !!!! IMPORTANT CHANGE !!!! Duplicate interests will create incoming face entry !!!! //
  //                                                                                     //
  /////////////////////////////////////////////////////////////////////////////////////////
  pitEntry->AddIncoming (incomingFace);
  m_dropInterests (header, incomingFace);
}

void
ForwardingStrategy::DidExhaustForwardingOptions (const Ptr<Face> &incomingFace,
                                                     Ptr<InterestHeader> header,
                                                     const Ptr<const Packet> &packet,
                                                     Ptr<pit::Entry> pitEntry)
{
  NS_LOG_FUNCTION (this << boost::cref (*incomingFace));
  m_dropInterests (header, incomingFace);
}

void
ForwardingStrategy::FailedToCreatePitEntry (const Ptr<Face> &incomingFace,
                                                Ptr<InterestHeader> header,
                                                const Ptr<const Packet> &packet)
{
  NS_LOG_FUNCTION (this);
  m_dropInterests (header, incomingFace);
}
  
void
ForwardingStrategy::DidCreatePitEntry (const Ptr<Face> &incomingFace,
                                           Ptr<InterestHeader> header,
                                           const Ptr<const Packet> &packet,
                                           Ptr<pit::Entry> pitEntrypitEntry)
{
}

bool
ForwardingStrategy::DetectRetransmittedInterest (const Ptr<Face> &incomingFace,
                                                     Ptr<pit::Entry> pitEntry)
{
  pit::Entry::in_iterator inFace = pitEntry->GetIncoming ().find (incomingFace);

  bool isRetransmitted = false;
  
  if (inFace != pitEntry->GetIncoming ().end ())
    {
      // this is almost definitely a retransmission. But should we trust the user on that?
      isRetransmitted = true;
    }

  return isRetransmitted;
}

void
ForwardingStrategy::SatisfyPendingInterest (const Ptr<Face> &incomingFace,
                                                Ptr<const ContentObjectHeader> header,
                                                Ptr<const Packet> payload,
                                                const Ptr<const Packet> &packet,
                                                Ptr<pit::Entry> pitEntry)
{
  if (incomingFace != 0)
    pitEntry->RemoveIncoming (incomingFace);

  //satisfy all pending incoming Interests
  BOOST_FOREACH (const pit::IncomingFace &incoming, pitEntry->GetIncoming ())
    {
      bool ok = incoming.m_face->Send (packet->Copy ());
      if (ok)
        {
          m_outData (header, payload, incomingFace == 0, incoming.m_face);
          DidSendOutData (incoming.m_face, header, payload, packet);
          
          NS_LOG_DEBUG ("Satisfy " << *incoming.m_face);
        }
      else
        {
          m_dropData (header, payload, incoming.m_face);
          NS_LOG_DEBUG ("Cannot satisfy data to " << *incoming.m_face);
        }
          
      // successfull forwarded data trace
    }

  // All incoming interests are satisfied. Remove them
  pitEntry->ClearIncoming ();

  // Remove all outgoing faces
  pitEntry->ClearOutgoing ();
          
  // Set pruning timout on PIT entry (instead of deleting the record)
  m_pit->MarkErased (pitEntry);
}

void
ForwardingStrategy::DidReceiveUnsolicitedData (const Ptr<Face> &incomingFace,
                                                   Ptr<const ContentObjectHeader> header,
                                                   Ptr<const Packet> payload)
{
  if (m_cacheUnsolicitedData)
    {
      // Optimistically add or update entry in the content store
      m_contentStore->Add (header, payload);
    }
  else
    {
      // Drop data packet if PIT entry is not found
      // (unsolicited data packets should not "poison" content store)
      
      //drop dulicated or not requested data packet
      m_dropData (header, payload, incomingFace);
    }
}

void
ForwardingStrategy::WillSatisfyPendingInterest (const Ptr<Face> &incomingFace,
                                                    Ptr<pit::Entry> pitEntry)
{
  pit::Entry::out_iterator out = pitEntry->GetOutgoing ().find (incomingFace);
  
  // If we have sent interest for this data via this face, then update stats.
  if (out != pitEntry->GetOutgoing ().end ())
    {
      pitEntry->GetFibEntry ()->UpdateFaceRtt (incomingFace, Simulator::Now () - out->m_sendTime);
    } 
}

bool
ForwardingStrategy::ShouldSuppressIncomingInterest (const Ptr<Face> &incomingFace,
                                                        Ptr<pit::Entry> pitEntry)
{
  bool isNew = pitEntry->GetIncoming ().size () == 0 && pitEntry->GetOutgoing ().size () == 0;

  if (isNew) return false; // never suppress new interests
  
  bool isRetransmitted = m_detectRetransmissions && // a small guard
                         DetectRetransmittedInterest (incomingFace, pitEntry);  

  if (pitEntry->GetOutgoing ().find (incomingFace) != pitEntry->GetOutgoing ().end ())
    {
      NS_LOG_DEBUG ("Non duplicate interests from the face we have sent interest to. Don't suppress");
      // got a non-duplicate interest from the face we have sent interest to
      // Probably, there is no point in waiting data from that face... Not sure yet

      // If we're expecting data from the interface we got the interest from ("producer" asks us for "his own" data)
      // Mark interface YELLOW, but keep a small hope that data will come eventually.

      // ?? not sure if we need to do that ?? ...
      
      // pitEntry->GetFibEntry ()->UpdateStatus (incomingFace, fib::FaceMetric::NDN_FIB_YELLOW);
    }
  else
    if (!isNew && !isRetransmitted)
      {
        return true;
      }

  return false;
}

void
ForwardingStrategy::PropagateInterest (const Ptr<Face> &incomingFace,
                                           Ptr<InterestHeader> header,
                                           const Ptr<const Packet> &packet,
                                           Ptr<pit::Entry> pitEntry)
{
  bool isRetransmitted = m_detectRetransmissions && // a small guard
                         DetectRetransmittedInterest (incomingFace, pitEntry);  

  //deal with the incoming with no existing PIT entry
  //add face to the list of incoming faces, added by Tang
  pitEntry->AddIncoming (incomingFace/*, header->GetInterestLifetime ()*/);
  
  /// @todo Make lifetime per incoming interface
  pitEntry->UpdateLifetime (header->GetInterestLifetime ());
  
  bool propagated = DoPropagateInterest (incomingFace, header, packet, pitEntry);

  if (!propagated && isRetransmitted) //give another chance if retransmitted
    {
      // increase max number of allowed retransmissions
      pitEntry->IncreaseAllowedRetxCount ();

      // try again
      propagated = DoPropagateInterest (incomingFace, header, packet, pitEntry);
    }

  // ForwardingStrategy will try its best to forward packet to at least one interface.
  // If no interests was propagated, then there is not other option for forwarding or
  // ForwardingStrategy failed to find it. 
  if (!propagated && pitEntry->GetOutgoing ().size () == 0)
    {
      DidExhaustForwardingOptions (incomingFace, header, packet, pitEntry);
    }
}

bool
ForwardingStrategy::WillSendOutInterest (const Ptr<Face> &outgoingFace,
                                             Ptr<InterestHeader> header,
                                             Ptr<pit::Entry> pitEntry)
{
  pit::Entry::out_iterator outgoing =
    pitEntry->GetOutgoing ().find (outgoingFace);
      
  if (outgoing != pitEntry->GetOutgoing ().end () &&
      outgoing->m_retxCount >= pitEntry->GetMaxRetxCount ())
    {
      NS_LOG_ERROR (outgoing->m_retxCount << " >= " << pitEntry->GetMaxRetxCount ());
      return false; // already forwarded before during this retransmission cycle
    }

  
  bool ok = outgoingFace->IsBelowLimit ();
  if (!ok)
    return false;

  pitEntry->AddOutgoing (outgoingFace);
  return true;
}

void
ForwardingStrategy::DidSendOutInterest (const Ptr<Face> &outgoingFace,
                                            Ptr<InterestHeader> header,
                                            const Ptr<const Packet> &packet,
                                            Ptr<pit::Entry> pitEntry)
{
  m_outInterests (header, outgoingFace);
}

void
ForwardingStrategy::DidSendOutData (const Ptr<Face> &face,
                                        Ptr<const ContentObjectHeader> header,
                                        Ptr<const Packet> payload,
                                        const Ptr<const Packet> &packet)
{
}

void
ForwardingStrategy::WillErasePendingInterest (Ptr<pit::Entry> pitEntry)
{
  // do nothing for now. may be need to do some logging
}


void
ForwardingStrategy::RemoveFace (Ptr<Face> face)
{
  // do nothing here
}

} // namespace ndn
} // namespace ns3
