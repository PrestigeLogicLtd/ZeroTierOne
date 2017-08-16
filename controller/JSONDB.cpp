/*
 * ZeroTier One - Network Virtualization Everywhere
 * Copyright (C) 2011-2015  ZeroTier, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#endif

#include "JSONDB.hpp"
#include "EmbeddedNetworkController.hpp"

namespace ZeroTier {

static const nlohmann::json _EMPTY_JSON(nlohmann::json::object());

JSONDB::JSONDB(const std::string &basePath,EmbeddedNetworkController *parent) :
	_parent(parent),
	_basePath(basePath),
	_rawInput(-1),
	_rawOutput(-1),
	_summaryThreadRun(true),
	_dataReady(false)
{
#ifndef __WINDOWS__
	if (_basePath == "-") {
		// If base path is "-" we run in Central harnessed mode. We read pseudo-http-requests from stdin and write
		// them to stdout.
		_rawInput = STDIN_FILENO;
		_rawOutput = STDOUT_FILENO;
		fcntl(_rawInput,F_SETFL,O_NONBLOCK);
	} else {
#endif
		// Default mode of operation is to store files in the filesystem
		OSUtils::mkdir(_basePath.c_str());
		OSUtils::lockDownFile(_basePath.c_str(),true); // networks might contain auth tokens, etc., so restrict directory permissions
#ifndef __WINDOWS__
	}
#endif

	_networks_m.lock(); // locked until data is loaded, etc.

	if (_rawInput < 0) {
		_load(basePath);
		_dataReady = true;
		_networks_m.unlock();
	} else {
		// In harnessed mode we leave the lock locked and wait for our initial DB from Central.
		_summaryThread = Thread::start(this);
	}
}

JSONDB::~JSONDB()
{
	Thread t;
	{
		Mutex::Lock _l(_summaryThread_m);
		_summaryThreadRun = false;
		t = _summaryThread;
	}
	if (t)
		Thread::join(t);
}

bool JSONDB::writeRaw(const std::string &n,const std::string &obj)
{
	if (_rawOutput >= 0) {
#ifndef __WINDOWS__
		if (obj.length() > 0) {
			Mutex::Lock _l(_rawLock);
			//fprintf(stderr,"%s\n",obj.c_str());
			if ((long)write(_rawOutput,obj.data(),obj.length()) == (long)obj.length()) {
				if (write(_rawOutput,"\n",1) == 1)
					return true;
			}
		} else return true;
#endif
		return false;
	} else {
		const std::string path(_genPath(n,true));
		if (!path.length())
			return false;
		return OSUtils::writeFile(path.c_str(),obj);
	}
}

bool JSONDB::hasNetwork(const uint64_t networkId) const
{
	Mutex::Lock _l(_networks_m);
	return (_networks.find(networkId) != _networks.end());
}

bool JSONDB::getNetwork(const uint64_t networkId,nlohmann::json &config) const
{
	Mutex::Lock _l(_networks_m);
	const std::unordered_map<uint64_t,_NW>::const_iterator i(_networks.find(networkId));
	if (i == _networks.end())
		return false;
	config = nlohmann::json::from_msgpack(i->second.config);
	return true;
}

bool JSONDB::getNetworkSummaryInfo(const uint64_t networkId,NetworkSummaryInfo &ns) const
{
	Mutex::Lock _l(_networks_m);
	const std::unordered_map<uint64_t,_NW>::const_iterator i(_networks.find(networkId));
	if (i == _networks.end())
		return false;
	ns = i->second.summaryInfo;
	return true;
}

int JSONDB::getNetworkAndMember(const uint64_t networkId,const uint64_t nodeId,nlohmann::json &networkConfig,nlohmann::json &memberConfig,NetworkSummaryInfo &ns) const
{
	Mutex::Lock _l(_networks_m);
	const std::unordered_map<uint64_t,_NW>::const_iterator i(_networks.find(networkId));
	if (i == _networks.end())
		return 0;
	const std::unordered_map< uint64_t,std::vector<uint8_t> >::const_iterator j(i->second.members.find(nodeId));
	if (j == i->second.members.end())
		return 1;
	networkConfig = nlohmann::json::from_msgpack(i->second.config);
	memberConfig = nlohmann::json::from_msgpack(j->second);
	ns = i->second.summaryInfo;
	return 3;
}

bool JSONDB::getNetworkMember(const uint64_t networkId,const uint64_t nodeId,nlohmann::json &memberConfig) const
{
	Mutex::Lock _l(_networks_m);
	const std::unordered_map<uint64_t,_NW>::const_iterator i(_networks.find(networkId));
	if (i == _networks.end())
		return false;
	const std::unordered_map< uint64_t,std::vector<uint8_t> >::const_iterator j(i->second.members.find(nodeId));
	if (j == i->second.members.end())
		return false;
	memberConfig = nlohmann::json::from_msgpack(j->second);
	return true;
}

void JSONDB::saveNetwork(const uint64_t networkId,const nlohmann::json &networkConfig)
{
	char n[64];
	OSUtils::ztsnprintf(n,sizeof(n),"network/%.16llx",(unsigned long long)networkId);
	writeRaw(n,OSUtils::jsonDump(networkConfig,-1));
	{
		Mutex::Lock _l(_networks_m);
		_NW &nw = _networks[networkId];
		nw.config = nlohmann::json::to_msgpack(networkConfig);
	}
	_recomputeSummaryInfo(networkId);
}

void JSONDB::saveNetworkMember(const uint64_t networkId,const uint64_t nodeId,const nlohmann::json &memberConfig)
{
	char n[256];
	OSUtils::ztsnprintf(n,sizeof(n),"network/%.16llx/member/%.10llx",(unsigned long long)networkId,(unsigned long long)nodeId);
	writeRaw(n,OSUtils::jsonDump(memberConfig,-1));
	{
		Mutex::Lock _l(_networks_m);
		std::vector<uint8_t> &m = _networks[networkId].members[nodeId];
		m = nlohmann::json::to_msgpack(memberConfig);
		_members[nodeId].insert(networkId);
	}
	_recomputeSummaryInfo(networkId);
}

nlohmann::json JSONDB::eraseNetwork(const uint64_t networkId)
{
	if (_rawOutput >= 0) {
		// In harnessed mode, DB deletes occur in the Central database and we do
		// not need to erase files.
	} else {
		std::vector<uint64_t> memberIds;
		{
			Mutex::Lock _l(_networks_m);
			const std::unordered_map<uint64_t,_NW>::iterator i(_networks.find(networkId));
			if (i == _networks.end())
				return _EMPTY_JSON;
			for(std::unordered_map< uint64_t,std::vector<uint8_t> >::iterator m(i->second.members.begin());m!=i->second.members.end();++m)
				memberIds.push_back(m->first);
		}
		for(std::vector<uint64_t>::iterator m(memberIds.begin());m!=memberIds.end();++m)
			eraseNetworkMember(networkId,*m,false);

		char n[256];
		OSUtils::ztsnprintf(n,sizeof(n),"network/%.16llx",(unsigned long long)networkId);
		const std::string path(_genPath(n,false));
		if (path.length())
			OSUtils::rm(path.c_str());
	}

	// This also erases all members from the memory cache
	{
		Mutex::Lock _l(_networks_m);
		std::unordered_map<uint64_t,_NW>::iterator i(_networks.find(networkId));
		if (i == _networks.end())
			return _EMPTY_JSON; // sanity check, shouldn't happen
		nlohmann::json tmp(nlohmann::json::from_msgpack(i->second.config));
		_networks.erase(i);
		return tmp;
	}
}

nlohmann::json JSONDB::eraseNetworkMember(const uint64_t networkId,const uint64_t nodeId,bool recomputeSummaryInfo)
{
	if (_rawOutput >= 0) {
		// In harnessed mode, DB deletes occur in Central and we do not remove files.
	} else {
		char n[256];
		OSUtils::ztsnprintf(n,sizeof(n),"network/%.16llx/member/%.10llx",(unsigned long long)networkId,(unsigned long long)nodeId);
		const std::string path(_genPath(n,false));
		if (path.length())
			OSUtils::rm(path.c_str());
	}

	{
		Mutex::Lock _l(_networks_m);
		_members[nodeId].erase(networkId);
		std::unordered_map<uint64_t,_NW>::iterator i(_networks.find(networkId));
		if (i == _networks.end())
			return _EMPTY_JSON;
		std::unordered_map< uint64_t,std::vector<uint8_t> >::iterator j(i->second.members.find(nodeId));
		if (j == i->second.members.end())
			return _EMPTY_JSON;
		nlohmann::json tmp(j->second);
		i->second.members.erase(j);
		if (recomputeSummaryInfo)
			_recomputeSummaryInfo(networkId);
		return tmp;
	}
}

void JSONDB::threadMain()
	throw()
{
#ifndef __WINDOWS__
	fd_set readfds,nullfds;
	char *const readbuf = (_rawInput >= 0) ? (new char[1048576]) : (char *)0;
	std::string rawInputBuf;
	FD_ZERO(&readfds);
	FD_ZERO(&nullfds);
	struct timeval tv;
#endif

	std::vector<uint64_t> todo;

	while (_summaryThreadRun) {
#ifndef __WINDOWS__
		if (_rawInput < 0) {
			Thread::sleep(25);
		} else {
			// In IPC mode we wait but also select() on STDIN to read database updates
			FD_SET(_rawInput,&readfds);
			tv.tv_sec = 0;
			tv.tv_usec = 25000;
			select(_rawInput+1,&readfds,&nullfds,&nullfds,&tv);
			if (FD_ISSET(_rawInput,&readfds)) {
				const long rn = (long)read(_rawInput,readbuf,1048576);
				bool gotMessage = false;
				for(long i=0;i<rn;++i) {
					if ((readbuf[i] != '\n')&&(readbuf[i] != '\r')&&(readbuf[i] != 0)) { // compatible with nodeJS IPC
						rawInputBuf.push_back(readbuf[i]);
					} else if (rawInputBuf.length() > 0) {
						try {
							const nlohmann::json obj(OSUtils::jsonParse(rawInputBuf));
							gotMessage = true;

							if (!_dataReady) {
								_dataReady = true;
								_networks_m.unlock();
							}

							if (obj.is_array()) {
								for(unsigned long i=0;i<obj.size();++i)
									_add(obj[i]);
							} else if (obj.is_object()) {
								_add(obj);
							}
						} catch ( ... ) {} // ignore malformed JSON

						rawInputBuf.clear();
					}
				}
				if (!gotMessage) // select() again immediately until we get at least one full message
					continue;
			}
		}
#else
		Thread::sleep(25);
#endif

		{
			Mutex::Lock _l(_summaryThread_m);
			if (_summaryThreadToDo.empty())
				continue;
			else _summaryThreadToDo.swap(todo);
		}

		if (!_dataReady) { // sanity check
			_dataReady = true;
			_networks_m.unlock();
		}

		const uint64_t now = OSUtils::now();
		try {
			Mutex::Lock _l(_networks_m);
			for(std::vector<uint64_t>::iterator ii(todo.begin());ii!=todo.end();++ii) {
				const uint64_t networkId = *ii;
				std::unordered_map<uint64_t,_NW>::iterator n(_networks.find(networkId));
				if (n != _networks.end()) {
					NetworkSummaryInfo &ns = n->second.summaryInfo;
					ns.activeBridges.clear();
					ns.allocatedIps.clear();
					ns.authorizedMemberCount = 0;
					ns.activeMemberCount = 0;
					ns.totalMemberCount = 0;
					ns.mostRecentDeauthTime = 0;

					for(std::unordered_map< uint64_t,std::vector<uint8_t> >::const_iterator m(n->second.members.begin());m!=n->second.members.end();++m) {
						try {
							nlohmann::json member(nlohmann::json::from_msgpack(m->second));

							if (OSUtils::jsonBool(member["authorized"],false)) {
								++ns.authorizedMemberCount;

								try {
									const nlohmann::json &mlog = member["recentLog"];
									if ((mlog.is_array())&&(mlog.size() > 0)) {
										const nlohmann::json &mlog1 = mlog[0];
										if (mlog1.is_object()) {
											if ((now - OSUtils::jsonInt(mlog1["ts"],0ULL)) < (ZT_NETWORK_AUTOCONF_DELAY * 2))
												++ns.activeMemberCount;
										}
									}
								} catch ( ... ) {}

								try {
									if (OSUtils::jsonBool(member["activeBridge"],false))
										ns.activeBridges.push_back(Address(m->first));
								} catch ( ... ) {}

								try {
									const nlohmann::json &mips = member["ipAssignments"];
									if (mips.is_array()) {
										for(unsigned long i=0;i<mips.size();++i) {
											InetAddress mip(OSUtils::jsonString(mips[i],"").c_str());
											if ((mip.ss_family == AF_INET)||(mip.ss_family == AF_INET6))
												ns.allocatedIps.push_back(mip);
										}
									}
								} catch ( ... ) {}
							} else {
								try {
									ns.mostRecentDeauthTime = std::max(ns.mostRecentDeauthTime,OSUtils::jsonInt(member["lastDeauthorizedTime"],0ULL));
								} catch ( ... ) {}
							}
							++ns.totalMemberCount;
						} catch ( ... ) {}
					}

					std::sort(ns.activeBridges.begin(),ns.activeBridges.end());
					std::sort(ns.allocatedIps.begin(),ns.allocatedIps.end());

					n->second.summaryInfoLastComputed = now;
				}
			}
		} catch ( ... ) {}

		todo.clear();
	}

	if (!_dataReady) // sanity check
		_networks_m.unlock();

#ifndef __WINDOWS__
	delete [] readbuf;
#endif
}

bool JSONDB::_add(const nlohmann::json &j)
{
	try {
		if (j.is_object()) {
			std::string id(OSUtils::jsonString(j["id"],"0"));
			std::string objtype(OSUtils::jsonString(j["objtype"],""));

			if ((id.length() == 16)&&(objtype == "network")) {
				const uint64_t nwid = Utils::hexStrToU64(id.c_str());
				if (nwid) {
					bool update;
					{
						Mutex::Lock _l(_networks_m);
						_NW &nw = _networks[nwid];
						update = !nw.config.empty();
						nw.config = nlohmann::json::to_msgpack(j);
					}
					if (update)
						_parent->onNetworkUpdate(nwid);
					_recomputeSummaryInfo(nwid);
					return true;
				}
			} else if ((id.length() == 10)&&(objtype == "member")) {
				const uint64_t mid = Utils::hexStrToU64(id.c_str());
				const uint64_t nwid = Utils::hexStrToU64(OSUtils::jsonString(j["nwid"],"0").c_str());
				if ((mid)&&(nwid)) {
					bool update;
					{
						Mutex::Lock _l(_networks_m);
						std::vector<uint8_t> &m = _networks[nwid].members[mid];
						update = !m.empty();
						m = nlohmann::json::to_msgpack(j);
						_members[mid].insert(nwid);
					}
					if (update)
						_parent->onNetworkMemberUpdate(nwid,mid);
					_recomputeSummaryInfo(nwid);
					return true;
				}
			}
		}
	} catch ( ... ) {}
	return false;
}

bool JSONDB::_load(const std::string &p)
{
	// This is not used in stdin/stdout mode. Instead data is populated by
	// sending it all to stdin.

	std::vector<std::string> dl(OSUtils::listDirectory(p.c_str(),true));
	for(std::vector<std::string>::const_iterator di(dl.begin());di!=dl.end();++di) {
		if ((di->length() > 5)&&(di->substr(di->length() - 5) == ".json")) {
			std::string buf;
			if (OSUtils::readFile((p + ZT_PATH_SEPARATOR_S + *di).c_str(),buf)) {
				try {
					_add(OSUtils::jsonParse(buf));
				} catch ( ... ) {}
			}
		} else {
			this->_load((p + ZT_PATH_SEPARATOR_S + *di));
		}
	}

	return true;
}

void JSONDB::_recomputeSummaryInfo(const uint64_t networkId)
{
	Mutex::Lock _l(_summaryThread_m);
	if (std::find(_summaryThreadToDo.begin(),_summaryThreadToDo.end(),networkId) == _summaryThreadToDo.end())
		_summaryThreadToDo.push_back(networkId);
	if (!_summaryThread)
		_summaryThread = Thread::start(this);
}

std::string JSONDB::_genPath(const std::string &n,bool create)
{
	std::vector<std::string> pt(OSUtils::split(n.c_str(),"/","",""));
	if (pt.size() == 0)
		return std::string();

	std::string p(_basePath);
	if (create) OSUtils::mkdir(p.c_str());
	for(unsigned long i=0,j=(unsigned long)(pt.size()-1);i<j;++i) {
		p.push_back(ZT_PATH_SEPARATOR);
		p.append(pt[i]);
		if (create) OSUtils::mkdir(p.c_str());
	}

	p.push_back(ZT_PATH_SEPARATOR);
	p.append(pt[pt.size()-1]);
	p.append(".json");

	return p;
}

} // namespace ZeroTier
