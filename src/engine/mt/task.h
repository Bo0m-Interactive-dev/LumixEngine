#pragma once

#include "engine/lumix.h"


namespace Lumix
{

struct IAllocator;

namespace MT
{

class CriticalSection;

class LUMIX_ENGINE_API Task
{
public:
	explicit Task(IAllocator& allocator);
	virtual ~Task();

	virtual int task() = 0;

	bool create(const char* name, bool is_extended);
	bool destroy();

	void setAffinityMask(u64 affinity_mask);

	// call only from task's thread
	void sleep(CriticalSection& cs);
	void wakeup();

	bool isRunning() const;
	bool isFinished() const;

protected:
	IAllocator& getAllocator();

private:
	struct TaskImpl* m_implementation;
};


} // namespace MT
} // namespace Lumix
