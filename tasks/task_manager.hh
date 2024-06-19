/*
 * Copyright (C) 2022-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include <list>
#include <boost/range/adaptors.hpp>
#include <boost/range/algorithm/transform.hpp>
#include <boost/range/join.hpp>
#include <seastar/core/on_internal_error.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/sleep.hh>
#include <seastar/coroutine/parallel_for_each.hh>
#include "db_clock.hh"
#include "log.hh"
#include "seastar/core/rwlock.hh"
#include "tasks/types.hh"
#include "utils/serialized_action.hh"
#include "utils/updateable_value.hh"

namespace repair {
class task_manager_module;
}

namespace tasks {

using is_abortable = bool_class <struct abortable_tag>;
using is_internal = bool_class<struct internal_tag>;

extern logging::logger tmlogger;

class task_manager : public peering_sharded_service<task_manager> {
public:
    class task;
    class module;
    struct config {
        utils::updateable_value<uint32_t> task_ttl;
    };
    using task_ptr = lw_shared_ptr<task_manager::task>;
    using task_map = std::unordered_map<task_id, task_ptr>;
    using foreign_task_ptr = foreign_ptr<task_ptr>;
    using foreign_task_map = std::unordered_map<task_id, foreign_task_ptr>;
    using module_ptr = shared_ptr<module>;
    using modules = std::unordered_map<std::string, module_ptr>;
private:
    task_map _all_tasks;
    modules _modules;
    config _cfg;
    seastar::abort_source _as;
    optimized_optional<seastar::abort_source::subscription> _abort_subscription;
    serialized_action _update_task_ttl_action;
    utils::observer<uint32_t> _task_ttl_observer;
    uint32_t _task_ttl;
public:
    class task_not_found : public std::exception {
        sstring _cause;
    public:
        explicit task_not_found(task_id tid)
            : _cause(format("task with id {} not found", tid))
        { }

        virtual const char* what() const noexcept override { return _cause.c_str(); }
    };

    enum class task_state {
        created,
        running,
        done,
        failed
    };

    class task : public enable_lw_shared_from_this<task> {
    public:
        struct progress {
            double completed = 0.0;         // Number of units completed so far.
            double total = 0.0;             // Total number of units to complete the task.

            progress& operator+=(const progress& rhs) {
                completed += rhs.completed;
                total += rhs.total;
                return *this;
            }

            friend progress operator+(progress lhs, const progress& rhs) {
                lhs += rhs;
                return lhs;
            }
        };

        struct status {
            task_id id;
            task_state state = task_state::created;
            db_clock::time_point start_time;
            db_clock::time_point end_time;
            std::string error;
            uint64_t sequence_number = 0;   // A running sequence number of the task.
            unsigned shard = 0;
            std::string scope;
            std::string keyspace;
            std::string table;
            std::string entity;             // Additional entity specific for the given type of task.
            std::string progress_units;     // A description of the units progress.
        };

        struct task_essentials {
            status task_status;
            progress task_progress;
            task_id parent_id;
            std::string type;
            is_abortable abortable;
            std::vector<task_essentials> failed_children;
        };

        class children {
            mutable foreign_task_map _children;
            mutable std::vector<task_essentials> _finished_children;
            mutable rwlock _lock;
        public:
            bool all_finished() const noexcept;
            size_t size() const noexcept;
            future<> add_child(foreign_task_ptr task);
            future<> mark_as_finished(task_id id, task_essentials essentials) const;
            future<progress> get_progress(const std::string& progress_units) const;
            future<> for_each_task(std::function<future<>(const foreign_task_ptr&)> f_children,
                    std::function<future<>(const task_essentials&)> f_finished_children) const;

            template<typename Res>
            future<std::vector<Res>> map_each_task(std::function<std::optional<Res>(const foreign_task_ptr&)> map_children,
                    std::function<std::optional<Res>(const task_essentials&)> map_finished_children) const {
                auto shared_holder = co_await _lock.hold_read_lock();

                co_return boost::copy_range<std::vector<Res>>(
                    boost::adaptors::filter(
                        boost::join(
                            _children | boost::adaptors::map_values | boost::adaptors::transformed(map_children),
                            _finished_children | boost::adaptors::transformed(map_finished_children)
                        ),
                        [] (const auto& task) { return bool(task); }
                    ) | boost::adaptors::transformed([] (const auto& res) { return res.value(); })
                );
            }
        };

        class impl {
        protected:
            status _status;
            task_id _parent_id;
            children _children;
            shared_promise<> _done;
            module_ptr _module;
            seastar::abort_source _as;
            optimized_optional<seastar::abort_source::subscription> _shutdown_subscription;
        public:
            impl(module_ptr module, task_id id, uint64_t sequence_number, std::string scope, std::string keyspace, std::string table, std::string entity, task_id parent_id) noexcept;
            // impl is always created as a smart pointer so it does not need to be moved or copied.
            impl(const impl&) = delete;
            impl(impl&&) = delete;
            virtual ~impl() = default;

            virtual std::string type() const = 0;
            virtual future<task_manager::task::progress> get_progress() const;
            virtual tasks::is_abortable is_abortable() const noexcept;
            virtual tasks::is_internal is_internal() const noexcept;
            virtual future<> abort() noexcept;
            bool is_complete() const noexcept;
            bool is_done() const noexcept;
            virtual void release_resources() noexcept {}
            future<std::vector<task_essentials>> get_failed_children() const;
        protected:
            virtual future<> run() = 0;
            void run_to_completion();
            future<> maybe_fold_into_parent() const noexcept;
            future<> finish() noexcept;
            future<> finish_failed(std::exception_ptr ex, std::string error) noexcept;
            future<> finish_failed(std::exception_ptr ex) noexcept;
            virtual future<std::optional<double>> expected_total_workload() const;
            virtual std::optional<double> expected_children_number() const;
            task_manager::task::progress get_binary_progress() const;

            friend task;
        };
        using task_impl_ptr = shared_ptr<impl>;
    protected:
        task_impl_ptr _impl;
    private:
        gate::holder _gate_holder;
    public:
        task(task_impl_ptr&& impl, gate::holder) noexcept;

        task_id id();
        std::string type() const;
        status& get_status() noexcept;
        uint64_t get_sequence_number() const noexcept;
        task_id get_parent_id() const noexcept;
        void change_state(task_state state) noexcept;
        future<> add_child(foreign_task_ptr&& child);
        void start();
        std::string get_module_name() const noexcept;
        module_ptr get_module() const noexcept;
        future<progress> get_progress() const;
        tasks::is_abortable is_abortable() const noexcept;
        tasks::is_internal is_internal() const noexcept;
        future<> abort() noexcept;
        bool abort_requested() const noexcept;
        future<> done() const noexcept;
        void register_task();
        void unregister_task() noexcept;
        const children& get_children() const noexcept;
        bool is_complete() const noexcept;
        future<std::vector<task_essentials>> get_failed_children() const;

        friend class test_task;
        friend class ::repair::task_manager_module;
    };

    class module : public enable_shared_from_this<module> {
    protected:
        task_manager& _tm;
        std::string _name;
        task_map _tasks;
        gate _gate;
        uint64_t _sequence_number = 0;
    private:
        abort_source _as;
        optimized_optional<abort_source::subscription> _abort_subscription;
    public:
        module(task_manager& tm, std::string name) noexcept;
        virtual ~module() = default;

        uint64_t new_sequence_number() noexcept;
        task_manager& get_task_manager() noexcept;
        seastar::abort_source& abort_source() noexcept;
        gate& async_gate() noexcept;
        const std::string& get_name() const noexcept;
        task_manager::task_map& get_tasks() noexcept;
        const task_manager::task_map& get_tasks() const noexcept;

        void register_task(task_ptr task);
        void unregister_task(task_id id) noexcept;
        virtual future<> stop() noexcept;
    public:
        template<typename T>
        requires std::is_base_of_v<task_manager::task::impl, T>
        future<task_id> make_task(unsigned shard, task_id id, std::string keyspace, std::string table, std::string entity, task_info parent_d) {
            return _tm.container().invoke_on(shard, [id, module = _name, keyspace = std::move(keyspace), table = std::move(table), entity = std::move(entity), parent_d] (task_manager& tm) {
                auto module_ptr = tm.find_module(module);
                auto task_impl_ptr = seastar::make_shared<T>(module_ptr, id ? id : task_id::create_random_id(), parent_d ? 0 : module_ptr->new_sequence_number(), std::move(keyspace), std::move(table), std::move(entity), parent_d.id);
                return module_ptr->make_task(std::move(task_impl_ptr), parent_d).then([] (auto task) {
                    return task->id();
                });
            });
        }

        // Must be called on target shard.
        // If task has a parent, data concerning its children is updated and sequence number is inherited
        // from a parent and set. Otherwise, it must be set by caller.
        future<task_ptr> make_task(task::task_impl_ptr task_impl_ptr, task_info parent_d = task_info{});

        // Must be called on target shard.
        template<typename TaskImpl, typename... Args>
        requires std::is_base_of_v<task::impl, TaskImpl> &&
        requires (module_ptr module, Args&&... args) {
            {TaskImpl(module, std::forward<Args>(args)...)} -> std::same_as<TaskImpl>;
        }
        future<task_ptr> make_and_start_task(tasks::task_info parent_info, Args&&... args) {
            auto task_impl_ptr = seastar::make_shared<TaskImpl>(shared_from_this(), std::forward<Args>(args)...);
            auto task = co_await make_task(std::move(task_impl_ptr), parent_info);
            task->start();
            co_return task;
        }
    };
public:
    task_manager(config cfg, seastar::abort_source& as) noexcept;
    task_manager() noexcept;

    modules& get_modules() noexcept;
    const modules& get_modules() const noexcept;
    task_map& get_all_tasks() noexcept;
    const task_map& get_all_tasks() const noexcept;

    module_ptr make_module(std::string name);
    void register_module(std::string name, module_ptr module);
    module_ptr find_module(std::string module_name);
    future<> stop() noexcept;

    static future<task_manager::foreign_task_ptr> lookup_task_on_all_shards(sharded<task_manager>& tm, task_id tid);
    static future<> invoke_on_task(sharded<task_manager>& tm, task_id id, std::function<future<> (task_manager::task_ptr)> func);
    template<typename T>
    static future<T> invoke_on_task(sharded<task_manager>& tm, task_id id, std::function<future<T> (task_manager::task_ptr)> func) {
        std::optional<T> res;
        co_await coroutine::parallel_for_each(boost::irange(0u, smp::count), [&tm, id, &res, &func] (unsigned shard) -> future<> {
            auto local_res = co_await tm.invoke_on(shard, [id, func] (const task_manager& local_tm) -> future<std::optional<T>> {
                const auto& all_tasks = local_tm.get_all_tasks();
                if (auto it = all_tasks.find(id); it != all_tasks.end()) {
                    co_return co_await func(it->second);
                }
                co_return std::nullopt;
            });
            if (!res) {
                res = std::move(local_res);
            } else if (local_res) {
                on_internal_error(tmlogger, format("task_id {} found on more than one shard", id));
            }
        });
        if (!res) {
            co_await coroutine::return_exception(task_manager::task_not_found(id));
        }
        co_return std::move(res.value());
    }

    seastar::abort_source& abort_source() noexcept;
protected:
    std::chrono::seconds get_task_ttl() const noexcept;
private:
    future<> update_task_ttl() noexcept {
        _task_ttl = _cfg.task_ttl.get();
        return make_ready_future<>();
    }
protected:
    void unregister_module(std::string name) noexcept;
    void register_task(task_ptr task);
    void unregister_task(task_id id) noexcept;
};

}
