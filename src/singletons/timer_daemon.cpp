// 这个文件是 Poseidon 服务器应用程序框架的一部分。
// Copyleft 2014 - 2017, LH_Mouse. All wrongs reserved.

#include "../precompiled.hpp"
#include "timer_daemon.hpp"
#include "job_dispatcher.hpp"
#include "../thread.hpp"
#include "../log.hpp"
#include "../atomic.hpp"
#include "../exception.hpp"
#include "../mutex.hpp"
#include "../condition_variable.hpp"
#include "../time.hpp"
#include "../job_base.hpp"
#include "../profiler.hpp"
#include "../checked_arithmetic.hpp"

namespace Poseidon {

struct TimerItem : NONCOPYABLE {
	boost::uint64_t period;
	boost::shared_ptr<const TimerCallback> callback;
	bool low_level;
	unsigned long stamp;

	TimerItem(boost::uint64_t period_, boost::shared_ptr<const TimerCallback> callback_, bool low_level_)
		: period(period_), callback(STD_MOVE(callback_)), low_level(low_level_)
		, stamp(0)
	{
		LOG_POSEIDON_DEBUG("Created timer: period = ", period, ", low_level = ", low_level);
	}
	~TimerItem(){
		LOG_POSEIDON_DEBUG("Destroyed timer: period = ", period, ", low_level = ", low_level);
	}
};

namespace {
	CONSTEXPR const boost::uint64_t MS_PER_HOUR = 3600000;
	CONSTEXPR const boost::uint64_t MS_PER_DAY  = MS_PER_HOUR * 24;
	CONSTEXPR const boost::uint64_t MS_PER_WEEK = MS_PER_DAY * 7;

	class TimerJob : public JobBase {
	private:
		const boost::weak_ptr<TimerItem> m_item;
		const boost::uint64_t m_now;

	public:
		TimerJob(boost::weak_ptr<TimerItem> item, boost::uint64_t now)
			: m_item(STD_MOVE(item)), m_now(now)
		{ }

	public:
		boost::weak_ptr<const void> get_category() const OVERRIDE {
			return m_item;
		}
		void perform() OVERRIDE {
			PROFILE_ME;

			const AUTO(item, m_item.lock());
			if(!item){
				return;
			}

			(*item->callback)(item, m_now, item->period);
		}
	};

	struct TimerQueueElement {
		boost::uint64_t next;
		boost::weak_ptr<TimerItem> item;
		unsigned long stamp;

		TimerQueueElement(boost::uint64_t next_, const boost::shared_ptr<TimerItem> &item_)
			: next(next_), item(item_), stamp(item_->stamp)
		{ }

		bool operator<(const TimerQueueElement &rhs) const {
			return next > rhs.next;
		}
	};

	inline void swap(TimerQueueElement &lhs, TimerQueueElement &rhs) NOEXCEPT {
		using std::swap;
		swap(lhs.next, rhs.next);
		swap(lhs.item, rhs.item);
		swap(lhs.stamp, rhs.stamp);
	}

	volatile bool g_running = false;
	Thread g_thread;

	Mutex g_mutex;
	ConditionVariable g_new_timer;
	std::vector<TimerQueueElement> g_timers;

	bool pump_one_element() NOEXCEPT {
		PROFILE_ME;

		const AUTO(now, get_fast_mono_clock());

		boost::shared_ptr<TimerItem> item;
		{
			const Mutex::UniqueLock lock(g_mutex);
		_pick_next:
			if(g_timers.empty()){
				return false;
			}
			if(now < g_timers.front().next){
				return false;
			}
			std::pop_heap(g_timers.begin(), g_timers.end());
			item = g_timers.back().item.lock();
			if(!item || (item->stamp != g_timers.back().stamp)){
				g_timers.pop_back();
				goto _pick_next;
			}
			if(item->period == 0){
				g_timers.pop_back();
			} else {
				g_timers.back().next = saturated_add(g_timers.back().next, item->period);
				std::push_heap(g_timers.begin(), g_timers.end());
			}
		}
		try {
			if(item->low_level){
				LOG_POSEIDON_TRACE("Dispatching low level timer: item = ", item);
				(*item->callback)(item, now, item->period);
			} else {
				LOG_POSEIDON_TRACE("Preparing a timer job for dispatching: item = ", item);
				JobDispatcher::enqueue(boost::make_shared<TimerJob>(item, now), VAL_INIT);
			}
		} catch(std::exception &e){
			LOG_POSEIDON_WARNING("std::exception thrown while dispatching timer job, what = ", e.what());
		} catch(...){
			LOG_POSEIDON_WARNING("Unknown exception thrown while dispatching timer job.");
		}
		return true;
	}

	void thread_proc(){
		PROFILE_ME;
		LOG_POSEIDON_INFO("Timer daemon started.");

		unsigned timeout = 0;
		for(;;){
			bool busy;
			do {
				busy = pump_one_element();
				timeout = std::min(timeout * 2u + 1u, !busy * 100u);
			} while(busy);

			Mutex::UniqueLock lock(g_mutex);
			if(!atomic_load(g_running, ATOMIC_CONSUME)){
				break;
			}
			g_new_timer.timed_wait(lock, timeout);
		}

		LOG_POSEIDON_INFO("Timer daemon stopped.");
	}
}

void TimerDaemon::start(){
	if(atomic_exchange(g_running, true, ATOMIC_ACQ_REL) != false){
		LOG_POSEIDON_FATAL("Only one daemon is allowed at the same time.");
		std::abort();
	}
	LOG_POSEIDON(Logger::SP_MAJOR | Logger::LV_INFO, "Starting timer daemon...");

	Thread(thread_proc, "  T ").swap(g_thread);
}
void TimerDaemon::stop(){
	if(atomic_exchange(g_running, false, ATOMIC_ACQ_REL) == false){
		return;
	}
	LOG_POSEIDON(Logger::SP_MAJOR | Logger::LV_INFO, "Stopping timer daemon...");

	if(g_thread.joinable()){
		g_thread.join();
	}
	g_timers.clear();
}

boost::shared_ptr<TimerItem> TimerDaemon::register_absolute_timer(
	boost::uint64_t first, boost::uint64_t period, TimerCallback callback)
{
	PROFILE_ME;

	AUTO(item, boost::make_shared<TimerItem>(period, boost::make_shared<TimerCallback>(STD_MOVE_IDN(callback)), false));
	{
		const Mutex::UniqueLock lock(g_mutex);
		g_timers.push_back(TimerQueueElement(first, item));
		std::push_heap(g_timers.begin(), g_timers.end());
		g_new_timer.signal();
	}
	LOG_POSEIDON_DEBUG("Created a timer which will be triggered ", saturated_sub(first, get_fast_mono_clock()),
		" microsecond(s) later and has a period of ", item->period, " microsecond(s).");
	return item;
}
boost::shared_ptr<TimerItem> TimerDaemon::register_timer(
	boost::uint64_t delta_first, boost::uint64_t period, TimerCallback callback)
{
	const AUTO(now, get_fast_mono_clock());
	return register_absolute_timer(saturated_add(now, delta_first), period, STD_MOVE(callback));
}

boost::shared_ptr<TimerItem> TimerDaemon::register_hourly_timer(
	unsigned minute, unsigned second, TimerCallback callback, bool utc)
{
	const AUTO(virt_now, utc ? get_utc_time() : get_local_time());
	const AUTO(delta, checked_sub(virt_now, (minute * 60ul + second) * 1000));
	return register_timer(MS_PER_HOUR - delta % MS_PER_HOUR, MS_PER_HOUR, STD_MOVE(callback));
}
boost::shared_ptr<TimerItem> TimerDaemon::register_daily_timer(
	unsigned hour, unsigned minute, unsigned second, TimerCallback callback, bool utc)
{
	const AUTO(virt_now, utc ? get_utc_time() : get_local_time());
	const AUTO(delta, checked_sub(virt_now, (hour * 3600ul + minute * 60ul + second) * 1000));
	return register_timer(MS_PER_DAY - delta % MS_PER_DAY, MS_PER_DAY, STD_MOVE(callback));
}
boost::shared_ptr<TimerItem> TimerDaemon::register_weekly_timer(
	unsigned day_of_week, unsigned hour, unsigned minute, unsigned second, TimerCallback callback, bool utc)
{
	// 注意 1970-01-01 是星期四。
	const AUTO(virt_now, utc ? get_utc_time() : get_local_time());
	const AUTO(delta, checked_sub(virt_now, ((day_of_week + 3) * 86400ul + hour * 3600ul + minute * 60ul + second) * 1000ul));
	return register_timer(MS_PER_WEEK - delta % MS_PER_WEEK, MS_PER_WEEK, STD_MOVE(callback));
}

boost::shared_ptr<TimerItem> TimerDaemon::register_low_level_absolute_timer(
	boost::uint64_t first, boost::uint64_t period, TimerCallback callback)
{
	PROFILE_ME;

	AUTO(item, boost::make_shared<TimerItem>(period, boost::make_shared<TimerCallback>(STD_MOVE_IDN(callback)), true));
	{
		const Mutex::UniqueLock lock(g_mutex);
		g_timers.push_back(TimerQueueElement(first, item));
		std::push_heap(g_timers.begin(), g_timers.end());
		g_new_timer.signal();
	}
	LOG_POSEIDON_DEBUG("Created a low level timer which will be triggered ", saturated_sub(first, get_fast_mono_clock()),
		" microsecond(s) later and has a period of ", item->period, " microsecond(s).");
	return item;
}
boost::shared_ptr<TimerItem> TimerDaemon::register_low_level_timer(
	boost::uint64_t delta_first, boost::uint64_t period, TimerCallback callback)
{
	const AUTO(now, get_fast_mono_clock());
	return register_low_level_absolute_timer(saturated_add(now, delta_first), period, STD_MOVE(callback));
}

void TimerDaemon::set_absolute_time(const boost::shared_ptr<TimerItem> &item,
	boost::uint64_t first, boost::uint64_t period)
{
	PROFILE_ME;

	const Mutex::UniqueLock lock(g_mutex);
	if(period != PERIOD_NOT_MODIFIED){
		item->period = period;
	}
	g_timers.push_back(TimerQueueElement(first, item)); // may throw std::bad_alloc.
	g_timers.back().stamp = ++(item->stamp);
	std::push_heap(g_timers.begin(), g_timers.end());
	g_new_timer.signal();
}
void TimerDaemon::set_time(const boost::shared_ptr<TimerItem> &item,
	boost::uint64_t delta_first, boost::uint64_t period)
{
	const AUTO(now, get_fast_mono_clock());
	return set_absolute_time(item, saturated_add(now, delta_first), period);
}

}
