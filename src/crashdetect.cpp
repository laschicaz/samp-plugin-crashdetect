// Copyright (c) 2011-2012, Zeex
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <cassert>
#include <cstdarg>
#include <cstdlib>
#include <list>
#include <map>
#include <stack>
#include <string>

#include <amx/amx.h>
#include <amx/amxaux.h>

#include "amxdebuginfo.h"
#include "amxpathfinder.h"
#include "amxstacktrace.h"
#include "amxutils.h"
#include "compiler.h"
#include "configreader.h"
#include "crashdetect.h"
#include "fileutils.h"
#include "logprintf.h"
#include "npcall.h"
#include "os.h"
#include "stacktrace.h"

using namespace amxutils;

#define AMX_EXEC_GDK (-10)

bool crashdetect::errorCaught_ = false;
std::stack<NPCall*> crashdetect::npCalls_;
ConfigReader crashdetect::serverCfg("server.cfg");
crashdetect::InstanceMap crashdetect::instances_;

// static
crashdetect *crashdetect::GetInstance(AMX *amx) {
	InstanceMap::const_iterator iterator = instances_.find(amx);
	if (iterator == instances_.end()) {
		crashdetect *c = new crashdetect(amx);
		instances_.insert(std::make_pair(amx, c));
		return c;
	}
	return iterator->second;
}

// static
void crashdetect::DestroyInstance(AMX *amx) {
	instances_.erase(amx);
}

// static
void crashdetect::SystemException(void *context) {
	if (!npCalls_.empty()) {
		AMX *amx = npCalls_.top()->amx();
		GetInstance(amx)->HandleException();
	} else {
		logprintf("Server crashed due to an unknown error");
	}
	PrintSystemBacktrace(context);
}

// static
void crashdetect::SystemInterrupt(void *context) {
	if (!npCalls_.empty()) {
		AMX *amx = npCalls_.top()->amx();
		GetInstance(amx)->HandleInterrupt();
	} else {
		logprintf("Server received interrupt signal");
	}
	PrintSystemBacktrace(context);
}

// static
void crashdetect::DieOrContinue() {
	if (serverCfg.GetOption("die_on_error", false)) {
		logprintf("Aborting...");
		std::exit(EXIT_FAILURE);
	}
}

crashdetect::crashdetect(AMX *amx)
	: amx_(amx)
	, amxhdr_(reinterpret_cast<AMX_HEADER*>(amx->base))
{
	AMXPathFinder pathFinder;
	pathFinder.AddSearchPath("gamemodes");
	pathFinder.AddSearchPath("filterscripts");

	// Read a list of additional search paths from AMX_PATH.
	const char *AMX_PATH = getenv("AMX_PATH");
	if (AMX_PATH != 0) {
		std::string var(AMX_PATH);
		std::string path;
		std::string::size_type begin = 0;
		while (begin < var.length()) {
			std::string::size_type end = var.find(fileutils::kNativePathListSepChar, begin);
			if (end == std::string::npos) {
				end = var.length();
			}
			path.assign(var.begin() + begin, var.begin() + end);
			if (!path.empty()) {
				pathFinder.AddSearchPath(path);
			}
			begin = end + 1;
		}
	}

	amxPath_ = pathFinder.FindAMX(amx_);
	amxName_ = fileutils::GetFileName(amxPath_);

	if (!amxPath_.empty() && AMXDebugInfo::IsPresent(amx_)) {
		debugInfo_.Load(amxPath_);
	}

	amx_->sysreq_d = 0;
	prevCallback_ = amx_->callback;
}

int crashdetect::DoAmxCallback(cell index, cell *result, cell *params) {
	NPCall call = NPCall::Native(amx_, index);

	npCalls_.push(&call);
	int retcode = prevCallback_(amx_, index, result, params);
	npCalls_.pop();

	return retcode;
}

int crashdetect::DoAmxExec(cell *retval, int index) {
	NPCall call = NPCall::Public(amx_, index);

	npCalls_.push(&call);

	int retcode = ::amx_Exec(amx_, retval, index);
	if (retcode != AMX_ERR_NONE && !errorCaught_) {
		amx_Error(amx_, index, retcode);
	} else {
		errorCaught_ = false;
	}

	npCalls_.pop();

	return retcode;
}

int crashdetect::DoAmxRelease(cell amx_addr, void *releaser) {
	if (amx_addr < amx_->hlw || amx_addr >= amx_->stk) {
		HandleReleaseError(amx_addr, releaser);
	}
	return amx_Release(amx_, amx_addr);
}

void crashdetect::HandleExecError(int index, int error) {
	crashdetect::errorCaught_ = true;

	if (error == AMX_ERR_INDEX && index == AMX_EXEC_GDK) {
		error = AMX_ERR_NONE;
		return;
	}

	logprintf("Run time error %d: \"%s\"", error, aux_StrError(error));

	switch (error) {
		case AMX_ERR_BOUNDS: {
			cell bound = *(reinterpret_cast<cell*>(amx_->cip + amx_->base + amxhdr_->cod + sizeof(cell)));
			cell index = amx_->pri;
			if (index < 0) {
				logprintf(" Accessing element at negative index %d", index);
			} else {
				logprintf(" Accessing element at index %d past array upper bound %d", index, bound);
			}
			break;
		}
		case AMX_ERR_NOTFOUND: {
			AMX_FUNCSTUBNT *natives = reinterpret_cast<AMX_FUNCSTUBNT*>(amx_->base + amxhdr_->natives);
			int numNatives = 0;
			amx_NumNatives(amx_, &numNatives);
			for (int i = 0; i < numNatives; ++i) {
				if (natives[i].address == 0) {
					char *name = reinterpret_cast<char*>(natives[i].nameofs + amx_->base);
					logprintf(" %s", name);
				}
			}
			break;
		}
		case AMX_ERR_STACKERR:
			logprintf(" Stack pointer (STK) is 0x%X, heap pointer (HEA) is 0x%X", amx_->stk, amx_->hea);
			break;
		case AMX_ERR_STACKLOW:
			logprintf(" Stack pointer (STK) is 0x%X, stack top (STP) is 0x%X", amx_->stk, amx_->stp);
			break;
		case AMX_ERR_HEAPLOW:
			logprintf(" Heap pointer (HEA) is 0x%X, heap bottom (HLW) is 0x%X", amx_->hea, amx_->hlw);
			break;
		case AMX_ERR_INVINSTR: {
			cell opcode = *(reinterpret_cast<cell*>(amx_->cip + amx_->base + amxhdr_->cod));
			logprintf(" Unknown opcode 0x%x at address 0x%08X", opcode , amx_->cip);
			break;
		}
	}

	if (error != AMX_ERR_NOTFOUND &&
		error != AMX_ERR_INDEX &&
		error != AMX_ERR_CALLBACK &&
		error != AMX_ERR_INIT)
	{
		PrintAmxBacktrace();
	}

	std::string command = serverCfg.GetOption("run_on_error", std::string());
	if (!command.empty()) {
		std::system(command.c_str());
	}

	DieOrContinue();
}

void crashdetect::HandleException() {
	logprintf("Server crashed while executing %s", amxName_.c_str());
	PrintAmxBacktrace();
}

void crashdetect::HandleInterrupt() {
	logprintf("Server received interrupt signal while executing %s", amxName_.c_str());
	PrintAmxBacktrace();
}

void crashdetect::HandleReleaseError(cell address, void *releaser) {
	std::string plugin = fileutils::GetFileName(os::GetModulePathFromAddr(releaser));
	if (plugin.empty()) {
		plugin.assign("<unknown>");
	}
	logprintf("Bad heap release detected:");
	logprintf(" %s [%08x] is releasing memory at %08x which is out of heap", plugin.c_str(), releaser, address);
	PrintSystemBacktrace();
}

// static
void crashdetect::PrintAmxBacktrace() {
	if (npCalls_.empty()) {
		return;
	}

	AMX *topAmx = npCalls_.top()->amx();

	cell frm = topAmx->frm;
	cell cip = topAmx->cip;
	int level = 0;

	if (cip == 0) {
		return;
	}

	logprintf("AMX backtrace:");

	std::stack<NPCall*> npCalls = npCalls_;

	while (!npCalls.empty() && cip != 0) {
		const NPCall *call = npCalls.top();

		// We don't trace calls across AMX bounds i.e. outside of top-level
		// function's AMX instance.
		if (call->amx() != topAmx) {
			assert(level != 0);
			break;
		}

		if (call->IsNative()) {
			AMX_NATIVE address = reinterpret_cast<AMX_NATIVE>(GetNativeAddr(call->amx(), call->index()));
			if (address != 0) {
				std::string module = fileutils::GetFileName(os::GetModulePathFromAddr((void*)address));
				std::string from = " from " + module;
				if (module.empty()) {
					from.clear();
				}
				const char *name = GetNativeName(call->amx(), call->index());
				if (name != 0) {
					logprintf("#%d native %s () [%08x]%s", level++, name, address, from.c_str());
				}
			}
		} else if (call->IsPublic()) {
			AMXDebugInfo &debugInfo = instances_[call->amx()]->debugInfo_;

			PushStack(call->amx(), cip); // push return address
			PushStack(call->amx(), frm); // push frame pointer

			std::list<AMXStackFrame> frames;
			{
				AMXStackTrace trace(call->amx(), call->amx()->stk, &debugInfo);
				do {
					AMXStackFrame frame = trace.GetFrame();
					if (frame) {
						frames.push_back(frame);
					} else {
						break;
					}
				} while (trace.Next());
			}

			frm = PopStack(call->amx()); // pop frame pointer
			cip = PopStack(call->amx()); // pop return address

			if (frames.empty()) {
				ucell epAddr = GetPublicAddr(call->amx(), call->index());
				frames.push_front(AMXStackFrame(call->amx(), frm, 0, epAddr, &debugInfo));
			} else {
				if (!debugInfo.IsLoaded()) {
					AMXStackFrame &bottom = frames.back();
					bottom = AMXStackFrame(call->amx(),
						bottom.GetFrameAddr(),
						bottom.GetRetAddr(),
						GetPublicAddr(call->amx(), call->index()),
						&debugInfo);
				}
			}

			std::string &amxName = instances_[call->amx()]->amxName_;
			for (std::list<AMXStackFrame>::const_iterator iterator = frames.begin();
					iterator != frames.end(); ++iterator)
			{
				std::string from = " from " + amxName;
				if (amxName.empty() || debugInfo.IsLoaded()) {
					from.clear();
				}
				logprintf("#%d %s%s", level++, iterator->AsString().c_str(), from.c_str());
			}

			frm = call->frm();
			cip = call->cip();
		}

		npCalls.pop();
	}
}

// static
void crashdetect::PrintSystemBacktrace(void *context) {
	logprintf("System backtrace:");

	int level = 0;

	StackTrace trace(context);
	std::deque<StackFrame> frames = trace.GetFrames();

	for (std::deque<StackFrame>::const_iterator iterator = frames.begin();
			iterator != frames.end(); ++iterator) {
		const StackFrame &frame = *iterator;

		std::string module = os::GetModulePathFromAddr(frame.GetRetAddr());
		std::string from = " from " + module;
		if (module.empty()) {
			from.clear();
		}

		logprintf("#%d %s%s", level++, frame.AsString().c_str(), from.c_str());
	}
}

// static
void crashdetect::logprintf(const char *format, ...) {
	std::va_list args;
	va_start(args, format);

	std::string prefixed_format;
	prefixed_format.append("[debug] ");
	prefixed_format.append(format);

	::vlogprintf(prefixed_format.c_str(), args);

	va_end(args);
}
