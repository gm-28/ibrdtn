/*
 * LinkManager.cpp
 *
 * Copyright (C) 2011 IBR, TU Braunschweig
 *
 * Written-by: Johannes Morgenroth <morgenroth@ibr.cs.tu-bs.de>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "ibrcommon/config.h"
#include "ibrcommon/link/LinkManager.h"
#include "ibrcommon/link/LinkEvent.h"
#include "ibrcommon/thread/MutexLock.h"
#include <list>
#include <string>
#include <typeinfo>

#ifdef HAVE_LIBNL
#include "ibrcommon/link/NetLinkManager.h"
#else
#ifdef HAVE_LIBNL3
#include "ibrcommon/link/NetLink3Manager.h"
#else
#include "ibrcommon/link/PosixLinkManager.h"
#endif
#endif

namespace ibrcommon
{
	LinkManager& LinkManager::getInstance()
	{
#ifdef HAVE_LIBNL
		static NetLinkManager lm;
#else
#ifdef HAVE_LIBNL3
		static NetLink3Manager lm;
#else
		static PosixLinkManager lm;
#endif
#endif

		return lm;
	}

	void LinkManager::initialize()
	{
		getInstance().up();
	}

	void LinkManager::registerInterfaceEvent(const vinterface &iface, LinkManager::EventCallback *cb)
	{
		if (cb == NULL) return;
		ibrcommon::MutexLock l(_listener_mutex);

		std::set<LinkManager::EventCallback* > &ss = _listener[iface];
		ss.insert(cb);
	}

	void LinkManager::unregisterInterfaceEvent(const vinterface &iface, LinkManager::EventCallback *cb)
	{
		if (cb == NULL) return;
		ibrcommon::MutexLock l(_listener_mutex);

		std::set<LinkManager::EventCallback* > &ss = _listener[iface];

		ss.erase(cb);

		if (ss.empty())
		{
			_listener.erase(iface);
		}
	}

	void LinkManager::unregisterAllEvents(LinkManager::EventCallback *cb)
	{
		if (cb == NULL) return;

		try {
			ibrcommon::MutexLock l(_listener_mutex);

			for (std::map<vinterface, std::set<LinkManager::EventCallback* > >::iterator iter = _listener.begin(); iter != _listener.end(); iter++)
			{
				std::set<LinkManager::EventCallback* > &ss = iter->second;
				ss.erase(cb);
			}
		} catch (const ibrcommon::MutexException&) {
			// this happens if this method is called after destroying the object
			// and is normal at shutdown
		}
	}

	void LinkManager::raiseEvent(const LinkEvent &lme)
	{
		// get the corresponding interface
		const vinterface &iface = lme.getInterface();

		// search for event subscriptions
		ibrcommon::MutexLock l(_listener_mutex);
		std::set<LinkManager::EventCallback* > &ss = _listener[iface];

		for (std::set<LinkManager::EventCallback* >::iterator iter = ss.begin(); iter != ss.end(); iter++)
		{
			try {
				(*iter)->eventNotify((LinkEvent&)lme);
			} catch (const std::exception&) { };
		}
	}

	void LinkManager::addressRemoved(const ibrcommon::vinterface &iface, const ibrcommon::vaddress &addr)
	{
		ManualLinkEvent evt(LinkEvent::EVENT_ADDRESS_REMOVED, iface, addr);
		raiseEvent(evt);
	}

	void LinkManager::addressAdded(const ibrcommon::vinterface &iface, const ibrcommon::vaddress &addr)
	{
		ManualLinkEvent evt(LinkEvent::EVENT_ADDRESS_ADDED, iface, addr);
		raiseEvent(evt);
	}
}