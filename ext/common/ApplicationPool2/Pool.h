/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2011-2015 Phusion
 *
 *  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 *  THE SOFTWARE.
 */
#ifndef _PASSENGER_APPLICATION_POOL2_POOL_H_
#define _PASSENGER_APPLICATION_POOL2_POOL_H_

#include <string>
#include <vector>
#include <algorithm>
#include <utility>
#include <sstream>
#include <iomanip>
#include <boost/thread.hpp>
#include <boost/bind.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/make_shared.hpp>
#include <boost/function.hpp>
#include <boost/foreach.hpp>
#include <boost/pool/object_pool.hpp>
// We use boost::container::vector instead of std::vector, because the
// former does not allocate memory in its default constructor. This is
// useful for post lock action vectors which often remain empty.
#include <boost/container/vector.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <oxt/dynamic_thread_group.hpp>
#include <oxt/backtrace.hpp>
#include <ApplicationPool2/Common.h>
#include <ApplicationPool2/Context.h>
#include <ApplicationPool2/Process.h>
#include <ApplicationPool2/Group.h>
#include <ApplicationPool2/Session.h>
#include <ApplicationPool2/Options.h>
#include <SpawningKit/Factory.h>
#include <MemoryKit/palloc.h>
#include <Logging.h>
#include <Exceptions.h>
#include <Hooks.h>
#include <Utils/Lock.h>
#include <Utils/AnsiColorConstants.h>
#include <Utils/SystemTime.h>
#include <Utils/MessagePassing.h>
#include <Utils/VariantMap.h>
#include <Utils/ProcessMetricsCollector.h>
#include <Utils/SystemMetricsCollector.h>

namespace Passenger {
namespace ApplicationPool2 {

using namespace std;
using namespace boost;
using namespace oxt;


class Pool: public boost::enable_shared_from_this<Pool> {
public:
	typedef void (*AbortLongRunningConnectionsCallback)(const ProcessPtr &process);

// Actually private, but marked public so that unit tests can access the fields.
public:
	friend class Group;
	friend class Process;
	friend struct tut::ApplicationPool2_PoolTest;

	mutable boost::mutex syncher;
	unsigned int max;
	unsigned long long maxIdleTime;
	bool selfchecking;

	Context context;

	/**
	 * Code can register background threads in one of these dynamic thread groups
	 * to ensure that threads are interrupted and/or joined properly upon Pool
	 * destruction.
	 * All threads in 'interruptableThreads' will be interrupted and joined upon
	 * Pool destruction.
	 * All threads in 'nonInterruptableThreads' will be joined, but not interrupted,
	 * upon Pool destruction.
	 */
	dynamic_thread_group interruptableThreads;
	dynamic_thread_group nonInterruptableThreads;

	enum LifeStatus {
		ALIVE,
		PREPARED_FOR_SHUTDOWN,
		SHUTTING_DOWN,
		SHUT_DOWN
	} lifeStatus;

	mutable GroupMap groups;
	psg_pool_t *palloc;

	/**
	 * get() requests that...
	 * - cannot be immediately satisfied because the pool is at full
	 *   capacity and no existing processes can be killed,
	 * - and for which the super group isn't in the pool,
	 * ...are put on this wait list.
	 *
	 * This wait list is processed when one of the following things happen:
	 *
	 * - A process has been spawned but its associated group has
	 *   no get waiters. This process can be killed and the resulting
	 *   free capacity will be used to spawn a process for this
	 *   get request.
	 * - A process (that has apparently been spawned after getWaitlist
	 *   was populated) is done processing a request. This process can
	 *   then be killed to free capacity.
	 * - A process has failed to spawn, resulting in capacity to
	 *   become free.
	 * - A Group failed to initialize, resulting in free capacity.
	 * - Someone commanded Pool to detach a process, resulting in free
	 *   capacity.
	 * - Someone commanded Pool to detach a Group, resulting in
	 *   free capacity.
	 * - The 'max' option has been increased, resulting in free capacity.
	 *
	 * Invariant 1:
	 *    for all options in getWaitlist:
	 *       options.getAppGroupName() is not in 'groups'.
	 *
	 * Invariant 2:
	 *    if getWaitlist is non-empty:
	 *       atFullCapacity()
	 * Equivalently:
	 *    if !atFullCapacity():
	 *       getWaitlist is empty.
	 */
	vector<GetWaiter> getWaitlist;

	const VariantMap *agentsOptions;

	#include <ApplicationPool2/Pool/AnalyticsCollection.cpp>
	#include <ApplicationPool2/Pool/GarbageCollection.cpp>
	#include <ApplicationPool2/Pool/GeneralUtils.cpp>
	#include <ApplicationPool2/Pool/ProcessUtils.cpp>
	#include <ApplicationPool2/Pool/Inspection.cpp>
	#include <ApplicationPool2/Pool/Debug.cpp>

// Actually private, but marked public so that unit tests can access the fields.
public:

	/** Process all waiters on the getWaitlist. Call when capacity has become free.
	 * This function assigns sessions to them by calling get() on the corresponding
	 * Groups, or by creating more Groups, in so far the new capacity allows.
	 */
	void assignSessionsToGetWaiters(boost::container::vector<Callback> &postLockActions) {
		bool done = false;
		vector<GetWaiter>::iterator it, end = getWaitlist.end();
		vector<GetWaiter> newWaitlist;

		for (it = getWaitlist.begin(); it != end && !done; it++) {
			GetWaiter &waiter = *it;

			Group *group = findMatchingGroup(waiter.options);
			if (group != NULL) {
				SessionPtr session = group->get(waiter.options, waiter.callback,
					postLockActions);
				if (session != NULL) {
					postLockActions.push_back(boost::bind(GetCallback::call,
						waiter.callback, session, ExceptionPtr()));
				}
				/* else: the callback has now been put in
				 *       the group's get wait list.
				 */
			} else if (!atFullCapacityUnlocked()) {
				createGroupAndAsyncGetFromIt(waiter.options, waiter.callback,
					postLockActions);
			} else {
				/* Still cannot satisfy this get request. Keep it on the get
				 * wait list and try again later.
				 */
				newWaitlist.push_back(waiter);
			}
		}

		std::swap(getWaitlist, newWaitlist);
	}

	template<typename Queue>
	static void assignExceptionToGetWaiters(Queue &getWaitlist,
		const ExceptionPtr &exception,
		boost::container::vector<Callback> &postLockActions)
	{
		while (!getWaitlist.empty()) {
			postLockActions.push_back(boost::bind(GetCallback::call,
				getWaitlist.front().callback, SessionPtr(),
				exception));
			getWaitlist.pop_front();
		}
	}

	void possiblySpawnMoreProcessesForExistingGroups() {
		/* Looks for Groups that are waiting for capacity to become available,
		 * and spawn processes in those groups.
		 */
		GroupMap::ConstIterator g_it(groups);
		while (*g_it != NULL) {
			const GroupPtr &group = g_it.getValue();
			if (group->isWaitingForCapacity()) {
				P_DEBUG("Group " << group->getName() << " is waiting for capacity");
				group->spawn();
				if (atFullCapacityUnlocked()) {
					return;
				}
			}
			g_it.next();
		}
		/* Now look for Groups that haven't maximized their allowed capacity
		 * yet, and spawn processes in those groups.
		 */
		g_it = GroupMap::ConstIterator(groups);
		while (*g_it != NULL) {
			const GroupPtr &group = g_it.getValue();
			if (group->shouldSpawn()) {
				P_DEBUG("Group " << group->getName() << " requests more processes to be spawned");
				group->spawn();
				if (atFullCapacityUnlocked()) {
					return;
				}
			}
			g_it.next();
		}
	}

	unsigned int capacityUsedUnlocked() const {
		if (groups.size() == 1) {
			GroupPtr *group;
			groups.lookupRandom(NULL, &group);
			return (*group)->capacityUsed();
		} else {
			GroupMap::ConstIterator g_it(groups);
			int result = 0;
			while (*g_it != NULL) {
				const GroupPtr &group = g_it.getValue();
				result += group->capacityUsed();
				g_it.next();
			}
			return result;
		}
	}

	bool atFullCapacityUnlocked() const {
		return capacityUsedUnlocked() >= max;
	}

	/**
	 * Calls Group::detach() so be sure to fix up the invariants afterwards.
	 * See the comments for Group::detach() and the code for detachProcessUnlocked().
	 */
	ProcessPtr forceFreeCapacity(const Group *exclude,
		boost::container::vector<Callback> &postLockActions)
	{
		ProcessPtr process = findOldestIdleProcess(exclude);
		if (process != NULL) {
			P_DEBUG("Forcefully detaching process " << process->inspect() <<
				" in order to free capacity in the pool");

			Group *group = process->getGroup();
			assert(group != NULL);
			assert(group->getWaitlist.empty());

			group->detach(process, postLockActions);
		}
		return process;
	}

	/**
	 * Forcefully destroys and detaches the given Group. After detaching
	 * the Group may have a non-empty getWaitlist so be sure to do
	 * something with it.
	 *
	 * Also, one of the post lock actions can potentially perform a long-running
	 * operation, so running them in a thread is advised.
	 */
	void forceDetachGroup(const GroupPtr &group,
		const Callback &callback,
		boost::container::vector<Callback> &postLockActions)
	{
		assert(group->getWaitlist.empty());
		const GroupPtr p = group; // Prevent premature destruction.
		bool removed = groups.erase(group->getName());
		assert(removed);
		(void) removed; // Shut up compiler warning.
		group->shutdown(callback, postLockActions);
	}

	bool detachProcessUnlocked(const ProcessPtr &process,
		boost::container::vector<Callback> &postLockActions)
	{
		if (OXT_LIKELY(process->isAlive())) {
			verifyInvariants();

			Group *group = process->getGroup();
			group->detach(process, postLockActions);
			// 'process' may now be a stale pointer so don't use it anymore.
			assignSessionsToGetWaiters(postLockActions);
			possiblySpawnMoreProcessesForExistingGroups();

			group->verifyInvariants();
			verifyInvariants();
			verifyExpensiveInvariants();

			return true;
		} else {
			return false;
		}
	}

	struct DetachGroupWaitTicket {
		boost::mutex syncher;
		boost::condition_variable cond;
		bool done;

		DetachGroupWaitTicket() {
			done = false;
		}
	};

	struct DisableWaitTicket {
		boost::mutex syncher;
		boost::condition_variable cond;
		DisableResult result;
		bool done;

		DisableWaitTicket() {
			done = false;
		}
	};

	static void syncDetachGroupCallback(boost::shared_ptr<DetachGroupWaitTicket> ticket) {
		LockGuard l(ticket->syncher);
		ticket->done = true;
		ticket->cond.notify_one();
	}

	static void waitDetachGroupCallback(boost::shared_ptr<DetachGroupWaitTicket> ticket) {
		ScopedLock l(ticket->syncher);
		while (!ticket->done) {
			ticket->cond.wait(l);
		}
	}

	static void syncDisableProcessCallback(const ProcessPtr &process, DisableResult result,
		boost::shared_ptr<DisableWaitTicket> ticket)
	{
		LockGuard l(ticket->syncher);
		ticket->done = true;
		ticket->result = result;
		ticket->cond.notify_one();
	}

	static void syncGetCallback(const SessionPtr &session, const ExceptionPtr &e,
		void *userData)
	{
		Ticket *ticket = static_cast<Ticket *>(userData);
		ScopedLock lock(ticket->syncher);
		if (OXT_LIKELY(session != NULL)) {
			ticket->session = session;
		} else {
			ticket->exception = e;
		}
		ticket->cond.notify_one();
	}

	Group *findMatchingGroup(const Options &options) {
		GroupPtr *group;
		if (groups.lookup(options.getAppGroupName(), &group)) {
			return group->get();
		} else {
			return NULL;
		}
	}

	GroupPtr createGroup(const Options &options) {
		GroupPtr group = boost::make_shared<Group>(this, options);
		group->initialize();
		groups.insert(options.getAppGroupName(), group);
		wakeupGarbageCollector();
		return group;
	}

	GroupPtr createGroupAndAsyncGetFromIt(const Options &options,
		const GetCallback &callback, boost::container::vector<Callback> &postLockActions)
	{
		GroupPtr group = createGroup(options);
		SessionPtr session = group->get(options, callback,
			postLockActions);
		/* If !options.noop, then the callback should now have been put on the
		 * wait list, unless something has changed and we forgot to update
		 * some code here...
		 */
		if (session != NULL) {
			assert(options.noop);
			postLockActions.push_back(boost::bind(GetCallback::call,
				callback, session, ExceptionPtr()));
		}
		return group;
	}

	// Debugging helper function, implemented in .cpp file so that GDB can access it.
	const GroupPtr getGroup(const char *name);

public:
	AbortLongRunningConnectionsCallback abortLongRunningConnectionsCallback;

	Pool(const SpawningKit::FactoryPtr &spawningKitFactory,
		const VariantMap *agentsOptions = NULL)
		: abortLongRunningConnectionsCallback(NULL)
	{
		context.setSpawningKitFactory(spawningKitFactory);
		context.finalize();

		this->agentsOptions = agentsOptions;

		try {
			systemMetricsCollector.collect(systemMetrics);
		} catch (const RuntimeException &e) {
			P_WARN("Unable to collect system metrics: " << e.what());
		}

		lifeStatus   = ALIVE;
		max          = 6;
		maxIdleTime  = 60 * 1000000;
		selfchecking = true;
		palloc       = psg_create_pool(PSG_DEFAULT_POOL_SIZE);

		// The following code only serve to instantiate certain inline methods
		// so that they can be invoked from gdb.
		(void) GroupPtr().get();
		(void) ProcessPtr().get();
		(void) SessionPtr().get();
	}

	~Pool() {
		if (lifeStatus != SHUT_DOWN) {
			P_BUG("You must call Pool::destroy() before actually destroying the Pool object!");
		}
		psg_destroy_pool(palloc);
	}

	/** Must be called right after construction. */
	void initialize() {
		LockGuard l(syncher);
		initializeAnalyticsCollection();
		initializeGarbageCollection();
	}

	void initDebugging() {
		LockGuard l(syncher);
		debugSupport = boost::make_shared<DebugSupport>();
	}

	/** Should be called right after the agent has received
	 * the message to exit gracefully. This will tell processes to
	 * abort any long-running connections, e.g. WebSocket connections,
	 * because the RequestHandler has to wait until all connections are
	 * finished before proceeding with shutdown.
	 */
	void prepareForShutdown() {
		TRACE_POINT();
		ScopedLock lock(syncher);
		assert(lifeStatus == ALIVE);
		lifeStatus = PREPARED_FOR_SHUTDOWN;
		if (abortLongRunningConnectionsCallback != NULL) {
			vector<ProcessPtr> processes = getProcesses(false);
			foreach (ProcessPtr process, processes) {
				// Ensure that the process is not immediately respawned.
				process->getGroup()->options.minProcesses = 0;
				abortLongRunningConnectionsCallback(process);
			}
		}
	}

	/** Must be called right before destruction. */
	void destroy() {
		TRACE_POINT();
		ScopedLock lock(syncher);
		assert(lifeStatus == ALIVE || lifeStatus == PREPARED_FOR_SHUTDOWN);

		lifeStatus = SHUTTING_DOWN;

		while (!groups.empty()) {
			GroupPtr *group;
			groups.lookupRandom(NULL, &group);
			string name = group->get()->getName().toString();
			lock.unlock();
			detachGroupByName(name);
			lock.lock();
		}

		UPDATE_TRACE_POINT();
		lock.unlock();
		interruptableThreads.interrupt_and_join_all();
		nonInterruptableThreads.join_all();
		lock.lock();

		lifeStatus = SHUT_DOWN;

		UPDATE_TRACE_POINT();
		verifyInvariants();
		verifyExpensiveInvariants();
	}

	// 'lockNow == false' may only be used during unit tests. Normally we
	// should never call the callback while holding the lock.
	void asyncGet(const Options &options, const GetCallback &callback, bool lockNow = true) {
		DynamicScopedLock lock(syncher, lockNow);

		assert(lifeStatus == ALIVE || lifeStatus == PREPARED_FOR_SHUTDOWN);
		verifyInvariants();
		P_TRACE(2, "asyncGet(appGroupName=" << options.getAppGroupName() << ")");
		boost::container::vector<Callback> actions;

		Group *existingGroup = findMatchingGroup(options);
		if (OXT_LIKELY(existingGroup != NULL)) {
			/* Best case: the app group is already in the pool. Let's use it. */
			P_TRACE(2, "Found existing Group");
			existingGroup->verifyInvariants();
			SessionPtr session = existingGroup->get(options, callback, actions);
			existingGroup->verifyInvariants();
			verifyInvariants();
			P_TRACE(2, "asyncGet() finished");
			if (lockNow) {
				lock.unlock();
			}
			if (session != NULL) {
				callback(session, ExceptionPtr());
			}

		} else if (!atFullCapacityUnlocked()) {
			/* The app super group isn't in the pool and we have enough free
			 * resources to make a new one.
			 */
			P_DEBUG("Spawning new Group");
			GroupPtr group = createGroupAndAsyncGetFromIt(options,
				callback, actions);
			group->verifyInvariants();
			verifyInvariants();
			P_DEBUG("asyncGet() finished");

		} else {
			/* Uh oh, the app super group isn't in the pool but we don't
			 * have the resources to make a new one. The sysadmin should
			 * configure the system to let something like this happen
			 * as least as possible, but let's try to handle it as well
			 * as we can.
			 */
			ProcessPtr freedProcess = forceFreeCapacity(NULL, actions);
			if (freedProcess == NULL) {
				/* No process is eligible for killing. This could happen if, for example,
				 * all (super)groups are currently initializing/restarting/spawning/etc.
				 * We have no choice but to satisfy this get() action later when resources
				 * become available.
				 */
				P_DEBUG("Could not free a process; putting request to top-level getWaitlist");
				getWaitlist.push_back(GetWaiter(
					options.copyAndPersist().detachFromUnionStationTransaction(),
					callback));
			} else {
				/* Now that a process has been trashed we can create
				 * the missing Group.
				 */
				P_DEBUG("Creating new Group");
				GroupPtr group = createGroup(options);
				SessionPtr session = group->get(options, callback,
					actions);
				/* The Group is now spawning a process so the callback
				 * should now have been put on the wait list,
				 * unless something has changed and we forgot to update
				 * some code here or if options.noop...
				 */
				if (session != NULL) {
					assert(options.noop);
					actions.push_back(boost::bind(GetCallback::call,
						callback, session, ExceptionPtr()));
				}
				freedProcess->getGroup()->verifyInvariants();
				group->verifyInvariants();
			}

			assert(atFullCapacityUnlocked());
			verifyInvariants();
			verifyExpensiveInvariants();
			P_TRACE(2, "asyncGet() finished");
		}

		if (!actions.empty()) {
			if (lockNow) {
				if (lock.owns_lock()) {
					lock.unlock();
				}
				runAllActions(actions);
			} else {
				// This state is not allowed. If we reach
				// here then it probably indicates a bug in
				// the test suite.
				abort();
			}
		}
	}

	// TODO: 'ticket' should be a boost::shared_ptr for interruption-safety.
	SessionPtr get(const Options &options, Ticket *ticket) {
		ticket->session.reset();
		ticket->exception.reset();

		GetCallback callback;
		callback.func = syncGetCallback;
		callback.userData = ticket;
		asyncGet(options, callback);

		ScopedLock lock(ticket->syncher);
		while (ticket->session == NULL && ticket->exception == NULL) {
			ticket->cond.wait(lock);
		}
		lock.unlock();

		if (OXT_LIKELY(ticket->session != NULL)) {
			SessionPtr session = ticket->session;
			ticket->session.reset();
			return session;
		} else {
			rethrowException(ticket->exception);
			return SessionPtr(); // Shut up compiler warning.
		}
	}

	GroupPtr findOrCreateGroup(const Options &options) {
		Options options2 = options;
		options2.noop = true;

		Ticket ticket;
		{
			LockGuard l(syncher);
			GroupPtr *group;
			if (!groups.lookup(options.getAppGroupName(), &group)) {
				// Forcefully create Group, don't care whether resource limits
				// actually allow it.
				createGroup(options);
			}
		}
		return get(options2, &ticket)->getGroup()->shared_from_this();
	}

	void setMax(unsigned int max) {
		ScopedLock l(syncher);
		assert(max > 0);
		fullVerifyInvariants();
		bool bigger = max > this->max;
		this->max = max;
		if (bigger) {
			/* If there are clients waiting for resources
			 * to become free, spawn more processes now that
			 * we have the capacity.
			 *
			 * We favor waiters on the pool over waiters on the
			 * the groups because the latter already have the
			 * resources to eventually complete. Favoring waiters
			 * on the pool should be fairer.
			 */
			boost::container::vector<Callback> actions;
			assignSessionsToGetWaiters(actions);
			possiblySpawnMoreProcessesForExistingGroups();

			fullVerifyInvariants();
			l.unlock();
			runAllActions(actions);
		} else {
			fullVerifyInvariants();
		}
	}

	void setMaxIdleTime(unsigned long long value) {
		LockGuard l(syncher);
		maxIdleTime = value;
		wakeupGarbageCollector();
	}

	void enableSelfChecking(bool enabled) {
		LockGuard l(syncher);
		selfchecking = enabled;
	}

	unsigned int capacityUsed() const {
		LockGuard l(syncher);
		return capacityUsedUnlocked();
	}

	bool atFullCapacity() const {
		LockGuard l(syncher);
		return atFullCapacityUnlocked();
	}

	vector<ProcessPtr> getProcesses(bool lock = true) const {
		DynamicScopedLock l(syncher, lock);
		vector<ProcessPtr> result;
		GroupMap::ConstIterator g_it(groups);
		while (*g_it != NULL) {
			const GroupPtr &group = g_it.getValue();
			ProcessList::const_iterator p_it;

			for (p_it = group->enabledProcesses.begin(); p_it != group->enabledProcesses.end(); p_it++) {
				result.push_back(*p_it);
			}
			for (p_it = group->disablingProcesses.begin(); p_it != group->disablingProcesses.end(); p_it++) {
				result.push_back(*p_it);
			}
			for (p_it = group->disabledProcesses.begin(); p_it != group->disabledProcesses.end(); p_it++) {
				result.push_back(*p_it);
			}

			g_it.next();
		}
		return result;
	}

	/**
	 * Returns the total number of processes in the pool, including all disabling and
	 * disabled processes, but excluding processes that are shutting down and excluding
	 * processes that are being spawned.
	 */
	unsigned int getProcessCount(bool lock = true) const {
		DynamicScopedLock l(syncher, lock);
		unsigned int result = 0;
		GroupMap::ConstIterator g_it(groups);
		while (*g_it != NULL) {
			const GroupPtr &group = g_it.getValue();
			result += group->getProcessCount();
			g_it.next();
		}
		return result;
	}

	unsigned int getGroupCount() const {
		LockGuard l(syncher);
		return groups.size();
	}

	GroupPtr findGroupBySecret(const string &secret, bool lock = true) const {
		DynamicScopedLock l(syncher, lock);
		GroupMap::ConstIterator g_it(groups);
		while (*g_it != NULL) {
			const GroupPtr &group = g_it.getValue();
			if (group->getSecret() == secret) {
				return group;
			}
			g_it.next();
		}
		return GroupPtr();
	}

	ProcessPtr findProcessByGupid(const StaticString &gupid, bool lock = true) const {
		vector<ProcessPtr> processes = getProcesses(lock);
		vector<ProcessPtr>::const_iterator it, end = processes.end();
		for (it = processes.begin(); it != end; it++) {
			const ProcessPtr &process = *it;
			if (process->getGupid() == gupid) {
				return process;
			}
		}
		return ProcessPtr();
	}

	ProcessPtr findProcessByPid(pid_t pid, bool lock = true) const {
		vector<ProcessPtr> processes = getProcesses(lock);
		vector<ProcessPtr>::const_iterator it, end = processes.end();
		for (it = processes.begin(); it != end; it++) {
			const ProcessPtr &process = *it;
			if (process->getPid() == pid) {
				return process;
			}
		}
		return ProcessPtr();
	}

	bool detachGroupByName(const string &name) {
		TRACE_POINT();
		ScopedLock l(syncher);
		GroupPtr group = groups.lookupCopy(name);

		if (OXT_LIKELY(group != NULL)) {
			P_ASSERT_EQ(group->getName(), name);
			UPDATE_TRACE_POINT();
			verifyInvariants();
			verifyExpensiveInvariants();

			boost::container::vector<Callback> actions;
			boost::shared_ptr<DetachGroupWaitTicket> ticket =
				boost::make_shared<DetachGroupWaitTicket>();
			ExceptionPtr exception = copyException(
				GetAbortedException("The containing Group was detached."));

			assignExceptionToGetWaiters(group->getWaitlist,
				exception, actions);
			forceDetachGroup(group,
				boost::bind(syncDetachGroupCallback, ticket),
				actions);
			possiblySpawnMoreProcessesForExistingGroups();

			verifyInvariants();
			verifyExpensiveInvariants();

			l.unlock();
			UPDATE_TRACE_POINT();
			runAllActions(actions);
			actions.clear();

			UPDATE_TRACE_POINT();
			ScopedLock l2(ticket->syncher);
			while (!ticket->done) {
				ticket->cond.wait(l2);
			}
			return true;
		} else {
			return false;
		}
	}

	bool detachGroupBySecret(const string &groupSecret) {
		ScopedLock l(syncher);
		GroupPtr group = findGroupBySecret(groupSecret, false);
		if (group != NULL) {
			string name = group->getName();
			group.reset();
			l.unlock();
			return detachGroupByName(name);
		} else {
			return false;
		}
	}

	bool detachProcess(const ProcessPtr &process) {
		ScopedLock l(syncher);
		boost::container::vector<Callback> actions;
		bool result = detachProcessUnlocked(process, actions);
		fullVerifyInvariants();
		l.unlock();
		runAllActions(actions);
		return result;
	}

	bool detachProcess(pid_t pid) {
		ScopedLock l(syncher);
		ProcessPtr process = findProcessByPid(pid, false);
		if (process != NULL) {
			boost::container::vector<Callback> actions;
			bool result = detachProcessUnlocked(process, actions);
			fullVerifyInvariants();
			l.unlock();
			runAllActions(actions);
			return result;
		} else {
			return false;
		}
	}

	bool detachProcess(const string &gupid) {
		ScopedLock l(syncher);
		ProcessPtr process = findProcessByGupid(gupid, false);
		if (process != NULL) {
			boost::container::vector<Callback> actions;
			bool result = detachProcessUnlocked(process, actions);
			fullVerifyInvariants();
			l.unlock();
			runAllActions(actions);
			return result;
		} else {
			return false;
		}
	}

	DisableResult disableProcess(const StaticString &gupid) {
		ScopedLock l(syncher);
		ProcessPtr process = findProcessByGupid(gupid, false);
		if (process != NULL) {
			Group *group = process->getGroup();
			// Must be a boost::shared_ptr to be interruption-safe.
			boost::shared_ptr<DisableWaitTicket> ticket = boost::make_shared<DisableWaitTicket>();
			DisableResult result = group->disable(process,
				boost::bind(syncDisableProcessCallback, _1, _2, ticket));
			group->verifyInvariants();
			group->verifyExpensiveInvariants();
			if (result == DR_DEFERRED) {
				l.unlock();
				ScopedLock l2(ticket->syncher);
				while (!ticket->done) {
					ticket->cond.wait(l2);
				}
				return ticket->result;
			} else {
				return result;
			}
		} else {
			return DR_NOOP;
		}
	}

	bool restartGroupByName(const StaticString &name, RestartMethod method = RM_DEFAULT) {
		ScopedLock l(syncher);
		GroupMap::ConstIterator g_it(groups);
		while (*g_it != NULL) {
			const GroupPtr &group = g_it.getValue();
			if (name == group->getName()) {
				if (!group->restarting()) {
					group->restart(group->options, method);
				}
				return true;
			}
			g_it.next();
		}

		return false;
	}

	unsigned int restartGroupsByAppRoot(const StaticString &appRoot, RestartMethod method = RM_DEFAULT) {
		ScopedLock l(syncher);
		GroupMap::ConstIterator g_it(groups);
		unsigned int result = 0;

		while (*g_it != NULL) {
			const GroupPtr &group = g_it.getValue();
			if (appRoot == group->options.appRoot) {
				result++;
				group->restart(group->options, method);
			}
			g_it.next();
		}

		return result;
	}

	/**
	 * Checks whether at least one process is being spawned.
	 */
	bool isSpawning(bool lock = true) const {
		DynamicScopedLock l(syncher, lock);
		GroupMap::ConstIterator g_it(groups);
		while (*g_it != NULL) {
			const GroupPtr &group = g_it.getValue();
			if (group->spawning()) {
				return true;
			}
			g_it.next();
		}
		return false;
	}
};


} // namespace ApplicationPool2
} // namespace Passenger

#endif /* _PASSENGER_APPLICATION_POOL2_POOL_H_ */
