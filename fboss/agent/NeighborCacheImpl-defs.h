/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include "fboss/agent/NeighborCacheImpl.h"
#include "fboss/agent/types.h"
#include "fboss/agent/ArpHandler.h"
#include "fboss/agent/IPv6Handler.h"
#include "fboss/agent/state/SwitchState.h"
#include "fboss/agent/state/Vlan.h"
#include "fboss/agent/state/ArpTable.h"
#include "fboss/agent/state/NdpTable.h"
#include "fboss/agent/state/NeighborEntry.h"
#include <folly/MacAddress.h>
#include <folly/IPAddress.h>
#include <folly/io/async/EventBase.h>
#include <folly/futures/Future.h>

namespace facebook { namespace fboss {

namespace ncachehelpers {

/*
 * Helper that we can run to check that the interface and vlan
 * for an entry still exist and are valid.
 */
template <typename NTable>
bool checkVlanAndIntf(
    const std::shared_ptr<SwitchState>& state,
    const typename NeighborCacheEntry<NTable>::EntryFields& fields,
    VlanID vlanID) {
  // Make sure vlan exists
  auto* vlan = state->getVlans()->getVlanIf(vlanID).get();
  if (!vlan) {
    // This VLAN no longer exists.  Just ignore the entry update.
    VLOG(3) << "VLAN " << vlanID << " deleted before entry " <<
      fields.ip << " --> " << fields.mac << " could be updated";
    return false;
  }

  // In case the interface subnets have changed, make sure the IP address
  // is still on a locally attached subnet
  if (!Interface::isIpAttached(fields.ip, fields.interfaceID, state)) {
    VLOG(3) << "interface subnets changed before entry " <<
      fields.ip << " --> " << fields.mac << " could be updated";
    return false;
  }

  return true;
}

}

template <typename NTable>
void NeighborCacheImpl<NTable>::programEntry(Entry* entry) {
  CHECK(!entry->isPending());

  auto fields = entry->getFields();
  auto vlanID = vlanID_;
  auto updateFn = [fields, vlanID](const std::shared_ptr<SwitchState>& state)
      -> std::shared_ptr<SwitchState> {
    if (!ncachehelpers::checkVlanAndIntf<NTable>(state, fields, vlanID)) {
      // Either the vlan or intf is no longer valid.
      return nullptr;
    }

    auto vlan = state->getVlans()->getVlanIf(vlanID).get();
    std::shared_ptr<SwitchState> newState{state};
    auto* table = vlan->template getNeighborTable<NTable>().get();
    auto node = table->getNodeIf(fields.ip);

    if (!node) {
      table = table->modify(&vlan, &newState);
      table->addEntry(fields);
      VLOG(2) << "Adding entry for " << fields.ip << " --> " << fields.mac;
    } else {
      if (node->getMac() == fields.mac &&
          node->getPort() == fields.port &&
          node->getIntfID() == fields.interfaceID &&
          !node->isPending()) {
        // This entry was already updated while we were waiting on the lock.
        return nullptr;
      }
      table = table->modify(&vlan, &newState);
      table->updateEntry(fields);
      VLOG(2) << "Converting pending entry for " << fields.ip << " --> "
                << fields.mac;
    }
    return newState;
  };

  sw_->updateState(folly::to<std::string>("add neighbor ", fields.ip),
                   std::move(updateFn));
}


template <typename NTable>
void NeighborCacheImpl<NTable>::programPendingEntry(Entry* entry, bool force) {
  CHECK(entry->isPending());

  auto fields = entry->getFields();
  auto vlanID = vlanID_;
  auto updateFn = [fields, vlanID, force](
    const std::shared_ptr<SwitchState>& state)
      -> std::shared_ptr<SwitchState> {
    if (!ncachehelpers::checkVlanAndIntf<NTable>(state, fields, vlanID)) {
      // Either the vlan or intf is no longer valid.
      return nullptr;
    }

    auto vlan = state->getVlans()->getVlanIf(vlanID).get();
    std::shared_ptr<SwitchState> newState{state};
    auto* table = vlan->template getNeighborTable<NTable>().get();
    auto node = table->getNodeIf(fields.ip);
    table = table->modify(&vlan, &newState);

    if (node) {
      if (!force) {
        // don't replace an existing entry with a pending one unless
        // explicitly allowed
        return nullptr;
      }
      table->removeEntry(fields.ip);
    }
    table->addPendingEntry(fields.ip, fields.interfaceID);

    VLOG(4) << "Adding pending entry for " << fields.ip
            << " on interface " << fields.interfaceID;
    return newState;
  };

  sw_->updateState(folly::to<std::string>("add pending entry ", fields.ip),
                   std::move(updateFn));
}

template <typename NTable>
NeighborCacheImpl<NTable>::~NeighborCacheImpl() {
  /* All the NeighborCacheEntries need to be destroyed on
   * the background thread. Because we do not want to exit
   * the destructor until all of the entries have stopped
   * executing, we use folly futures to wait for all entries
   * to destroy themselves.
   */
  std::vector<folly::Future<folly::Unit>> stopTasks;

  for (auto item : entries_) {
    auto addr = item.first;
    auto entry = item.second;

    std::function<void()> stopEntry = [this, entry]() {
      Entry::destroy(std::move(entry), sw_->getBackgroundEVB());
    };

    // Run the stop function in the background thread to
    // ensure it can be safely run
    auto f = via(sw_->getBackgroundEVB())
      .then(stopEntry)
      .onError([=](const std::exception& e) {
          LOG(FATAL) << "failed to stop NeighborCacheEntry w/ addr " << addr;
        });
    stopTasks.push_back(std::move(f));
  }

  // Ensure that all of the updaters have been stopped before we return
  folly::collectAll(stopTasks).get();
}

template <typename NTable>
void NeighborCacheImpl<NTable>::repopulate(std::shared_ptr<NTable> table) {
  for (auto it = table->begin(); it != table->end(); ++it) {
    auto entry = *it;
    auto state = entry->isPending() ? NeighborEntryState::INCOMPLETE :
      NeighborEntryState::STALE;
    setEntryInternal(*entry->getFields(), state, true);
  }
}

template <typename NTable>
void NeighborCacheImpl<NTable>::setEntry(AddressType ip,
                                         folly::MacAddress mac,
                                         PortID portID,
                                         NeighborEntryState state) {
  auto entry = setEntryInternal(EntryFields(ip, mac, portID, intfID_), state);
  programEntry(entry);
}

template <typename NTable>
void NeighborCacheImpl<NTable>::setExistingEntry(AddressType ip,
                                                 folly::MacAddress mac,
                                                 PortID portID,
                                                 NeighborEntryState state) {
  auto entry = setEntryInternal(
    EntryFields(ip, mac, portID, intfID_), state, false);
  if (entry) {
    // only program an entry if one exists
    programEntry(entry);
  }
}

template <typename NTable>
NeighborCacheEntry<NTable>* NeighborCacheImpl<NTable>::setEntryInternal(
    const EntryFields& fields,
    NeighborEntryState state,
    bool add) {
  auto entry = getCacheEntry(fields.ip);
  if (entry) {
    if (!entry->fieldsMatch(fields)) {
      entry->updateFields(fields);
    }
    entry->updateState(state);
  } else if (add) {
    auto evb = sw_->getBackgroundEVB();
    auto to_store = std::make_shared<Entry>(fields, evb, cache_, state);
    entry = to_store.get();
    setCacheEntry(std::move(to_store));
  }
  return entry;
}

template <typename NTable>
void NeighborCacheImpl<NTable>::setPendingEntry(AddressType ip,
                                                bool force) {
  if (!force && getCacheEntry(ip)) {
    // only overwrite an existing entry with a pending entry if we say it is
    // ok with the 'force' parameter
    return;
  }

  auto newEntry = setEntryInternal(
    EntryFields(ip, intfID_, PENDING), NeighborEntryState::INCOMPLETE, true);
  programPendingEntry(newEntry, force);
}

template <typename NTable>
void NeighborCacheImpl<NTable>::processEntry(AddressType ip) {
  auto entry = getCacheEntry(ip);
  if (entry) {
    entry->process();
    if (entry->getState() == NeighborEntryState::EXPIRED) {
      flushEntry(ip);
    }
  }
}

template <typename NTable>
NeighborCacheEntry<NTable>* NeighborCacheImpl<NTable>::getCacheEntry(
    AddressType ip) const {
  auto it = entries_.find(ip);
  if (it != entries_.end()) {
    return it->second.get();
  }
  return nullptr;
}

template <typename NTable>
void NeighborCacheImpl<NTable>::setCacheEntry(std::shared_ptr<Entry> entry) {
  entries_[entry->getIP()] = std::move(entry);
}

template <typename NTable>
bool NeighborCacheImpl<NTable>::removeEntry(AddressType ip) {
  auto it = entries_.find(ip);
  if (it == entries_.end()) {
    return false;
  }

  // This asynchronously destroys the entries. This is needed because
  // entries need to be destroyed on the background thread, but we
  // likely have the cache level lock here and the background thread could be
  // waiting for the lock. To avoid this deadlock scenario, we keep the entry
  // around in a shared_ptr for a bit longer and then destroy it later.
  Entry::destroy(std::move(it->second), sw_->getBackgroundEVB());

  entries_.erase(it);

  return true;
}

template <typename NTable>
bool NeighborCacheImpl<NTable>::flushEntryFromSwitchState(
    std::shared_ptr<SwitchState>* state,
    Vlan* vlan,
    AddressType ip) {
  // The entry should be removed from the cache before it is
  // flushed from the SwitchState
  DCHECK(!getCacheEntry(ip));

  auto* table = vlan->template getNeighborTable<NTable>().get();
  const auto& entry = table->getNodeIf(ip);
  if (!entry) {
    return false;
  }

  table = table->modify(&vlan, state);
  table->removeNode(ip);
  return true;
}

template <typename NTable>
bool NeighborCacheImpl<NTable>::flushEntryBlocking(AddressType ip) {
  return flushEntry(ip, true);
}

template <typename NTable>
bool NeighborCacheImpl<NTable>::flushEntry(AddressType ip,
                                           bool blocking) {
  // remove from cache
  if (!removeEntry(ip)) {
    return false;
  }

  // flush from SwitchState
  bool flushed{false};
  auto updateFn =
    [this, ip, &flushed](const std::shared_ptr<SwitchState>& state)
        -> std::shared_ptr<SwitchState> {
      std::shared_ptr<SwitchState> newState{state};
      auto* vlan = state->getVlans()->getVlan(vlanID_).get();
      if (flushEntryFromSwitchState(&newState, vlan, ip)) {
        flushed = true;
        return newState;
      }
      return nullptr;
  };
  if (blocking) {
    sw_->updateStateBlocking("flush neighbor entry", std::move(updateFn));
    return flushed;
  }

  sw_->updateState("remove neighbor entry", std::move(updateFn));
  return true;
}

template <typename NTable>
bool NeighborCacheImpl<NTable>::isSolicited(AddressType ip) {
  // For now we assume that all entries that are either INCOMPLETE or PROBE
  // were solicited. We are sending out a request for these states every second
  // and are actively waiting for a reply so this is a reasonable assumption.
  auto entry = getCacheEntry(ip);
  return entry && entry->isProbing();
}

template <typename NTable>
void NeighborCacheImpl<NTable>::portDown(PortID port) {
  for (auto item : entries_) {
    if (item.second->getPortID() != port) {
      continue;
    }

    // TODO(aeckert): It would be nicer if we could just mark this
    // entry stale on port down so we don't need to unprogram the
    // entry (for fast port flaps).  However, we have seen packet
    // losses if we start forwarding packets on a port up event before
    // we receive a neighbor reply so it may not be worth leaving it
    // programmed. Also we need to notify the HwSwitch for ECMP expand
    // when the port comes back up and changing an entry from pending
    // to reachable is how we currently do this.
    setPendingEntry(item.second->getIP(), true);
  }
}

}} // facebook::fboss