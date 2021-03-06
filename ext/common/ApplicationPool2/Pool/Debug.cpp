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

// This file is included inside the Pool class.

protected:

struct DebugSupport {
	/** Mailbox for the unit tests to receive messages on. */
	MessageBoxPtr debugger;
	/** Mailbox for the ApplicationPool code to receive messages on. */
	MessageBoxPtr messages;

	// Choose aspects to debug.
	bool restarting;
	bool spawning;
	bool oobw;
	bool testOverflowRequestQueue;
	bool detachedProcessesChecker;

	// The following fields may only be accessed by Pool.
	boost::mutex syncher;
	unsigned int spawnLoopIteration;

	DebugSupport() {
		debugger = boost::make_shared<MessageBox>();
		messages = boost::make_shared<MessageBox>();
		restarting = true;
		spawning   = true;
		oobw       = false;
		detachedProcessesChecker = false;
		testOverflowRequestQueue = false;
		spawnLoopIteration = 0;
	}
};

typedef boost::shared_ptr<DebugSupport> DebugSupportPtr;

DebugSupportPtr debugSupport;
