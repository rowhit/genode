/*
 * \brief  Fork bomb to stress Genode
 * \author Christian Helmuth
 * \author Norman Feske
 * \author Alexander Böttcher
 * \date   2007-08-16
 */

/*
 * Copyright (C) 2007-2016 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

#include <base/component.h>
#include <base/child.h>
#include <base/service.h>
#include <base/attached_rom_dataspace.h>
#include <init/child_policy.h>
#include <timer_session/connection.h>
#include <os/child_policy_dynamic_rom.h>

using namespace Genode;


class Bomb_child : public Child_policy
{
	private:

		Env              &_env;
		Binary_name const _binary_name;
		Name        const _label;
		size_t      const _ram_quota;

		/*
		 * Entry point used for serving the parent interface
		 */
		enum { STACK_SIZE =  2048 * sizeof(addr_t) };
		Rpc_entrypoint _ep { &_env.pd(), STACK_SIZE, "bomb_ep_child", false };

		Registry<Registered<Parent_service> > &_parent_services;

		Child_policy_dynamic_rom_file _config_policy { "config", _ep, &_env.ram() };

		Child _child { _env.rm(), _ep, *this };

	public:

		Bomb_child(Env                                   &env,
		           Name                            const &binary_name,
		           Name                            const &label,
		           size_t                                 ram_quota,
		           Registry<Registered<Parent_service> > &parent_services,
		           unsigned                               generation)
		:
			_env(env), _binary_name(binary_name), _label(label),
			_ram_quota(Child::effective_ram_quota(ram_quota)),
			_parent_services(parent_services)
		{
			String<64> config("<config generations=\"", generation, "\"/>");
			_config_policy.load(config.string(), config.length());
			_ep.activate();
		}

		~Bomb_child() { log(__PRETTY_FUNCTION__); }


		/****************************
		 ** Child-policy interface **
		 ****************************/

		Name name() const override { return _label; }

		Binary_name binary_name() const override { return _binary_name; }

		void init(Ram_session &ram, Ram_session_capability ram_cap) override
		{
			ram.ref_account(_env.ram_session_cap());
			_env.ram().transfer_quota(ram_cap, _ram_quota);
		}

		Ram_session               &ref_ram()       override { return _env.ram(); }
		Ram_session_capability ref_ram_cap() const override { return _env.ram_session_cap(); }

		Service &resolve_session_request(Service::Name const &service_name,
		                                 Session_state::Args const &args) override
		{
			Service *service = nullptr;

			/* check for config file request */
			if ((service = _config_policy.resolve_session_request(service_name.string(),
			                                                      args.string())))
				return *service;

			_parent_services.for_each([&] (Service &s) {
				if (!service && service_name == s.name())
					service = &s; });

			if (!service)
				throw Parent::Service_denied();

			return *service;
		}
};


typedef Registry<Registered<Bomb_child> > Children;


/**
 * Check if a program with the specified name already exists
 */
static bool child_name_exists(Children const &children,
                              Bomb_child::Name const &name)
{
	bool found = false;

	children.for_each([&] (Bomb_child const &child) {
		if (!found && child.name() == name)
			found = true; });

	return found;
}


/**
 * Create a unique name based on the filename
 *
 * If a program with the filename as name already exists, we
 * add a counting number as suffix.
 */
static Bomb_child::Name
unique_child_name(Children const &children, Bomb_child::Name const &binary_name,
                  unsigned const generation)
{
	/* serialize calls to this function */
	static Lock lock;
	Lock::Guard guard(lock);

	for (unsigned cnt = 1; ; cnt++) {

		/* if such a program name does not exist yet, we are happy */
		Bomb_child::Name const unique(binary_name, "_g", generation, ".", cnt);
		if (!child_name_exists(children, unique.string()))
			return unique;
	}
}


struct Bomb
{
	Genode::Env &env;

	Constructible<Timer::Connection> timer;

	Genode::Signal_handler<Bomb> signal_timeout  { env.ep(), *this, &Bomb::destruct_children };
	Genode::Signal_handler<Bomb> signal_resource { env.ep(), *this, &Bomb::resource_request };

	Attached_rom_dataspace config { env, "config" };

	unsigned round = 0;
	unsigned const rounds      = config.xml().attribute_value("rounds", 1U);
	unsigned const generation  = config.xml().attribute_value("generations", 1U);
	unsigned const children    = config.xml().attribute_value("children", 2U);
	unsigned const sleeptime   = config.xml().attribute_value("sleep", 2000U);
	unsigned long const demand = config.xml().attribute_value("demand", 1024UL * 1024);

	Heap heap { env.ram(), env.rm() };

	Children child_registry;

	/* names of services provided by the parent */
	const char *names[6] = { "RAM", "PD", "CPU", "ROM", "LOG", 0};
	Registry<Registered<Parent_service> > parent_services;

	void construct_children()
	{
		unsigned long avail = env.ram().avail();
		unsigned long amount = (avail - demand) / children;
		if (amount < (demand * children)) {
			log("I'm a leaf node - generation ", generation,
			    " - not enough memory.");
			return;
		}
		if (generation == 0) {
			log("I'm a leaf node - generation 0");
			return;
		}

		log("[", round, "] It's time to start all my children...");

		Bomb_child::Name const binary_name("bomb");

		for (unsigned i = children; i; --i) {
			new (heap)
				Registered<Bomb_child>(child_registry, env, binary_name,
				                       unique_child_name(child_registry,
				                                         binary_name,
				                                         generation - 1),
				                       amount, parent_services, generation - 1);
		}

		/* master if we have a timer connection */
		if (timer.constructed())
			timer->trigger_once(sleeptime * 1000);
	}

	void destruct_children()
	{
		log("[", round, "] It's time to kill all my children...");

		child_registry.for_each([&] (Registered<Bomb_child> &child) {
			destroy(heap, &child); });

		log("[", round, "] Done.");

		++round;

		/* master if we have a timer connection */
		if (round == rounds && timer.constructed()) {
			log("Done. Going to sleep");
			return;
		}

		construct_children();
	}

	void resource_request()
	{
		Genode::error("resource request");
	}

	Bomb(Genode::Env &env) : env(env)
	{
		for (unsigned i = 0; names[i]; i++)
			new (heap) Registered<Parent_service>(parent_services, names[i]);

		/*
		 * Don't ask parent for further resources if we ran out of memory.
		 * Prevent us to block for resource upgrades caused by clients
		 */
		env.parent().resource_avail_sigh(signal_resource);

		log("--- bomb started ---");

		/* try to create timer session, if it fails, bomb is our parent */
		try { timer.construct(env); } catch (Parent::Service_denied) { }

		if (timer.constructed()) {
			timer->sigh(signal_timeout);

			log("rounds=", rounds, " generations=", generation, " children=",
			    children, " sleep=", sleeptime, " demand=", demand/1024, "K");
		}

		construct_children();
	}
};

void Component::construct(Genode::Env &env) { static Bomb bomb(env); }
