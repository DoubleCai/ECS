#pragma once

/*
Copyright (c) 2016 Sam Bloomberg

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <unordered_map>
#include <functional>
#include <vector>
#include <algorithm>
#include <stdint.h>

//////////////////////////////////////////////////////////////////////////
// SETTINGS //
//////////////////////////////////////////////////////////////////////////


// Define what you want to pass to the tick() function by defining ECS_TICK_TYPE before including this header,
// or leave it as default (float).
// This is really messy to do but the alternative is some sort of slow custom event setup for ticks, which is silly.

// Add this before including this header if you don't want to pass anything to tick()
//#define ECS_TICK_TYPE_VOID
#ifndef ECS_TICK_TYPE
#define ECS_TICK_TYPE ECS::DefaultTickData
#endif

// Define what kind of allocator you want the world to use. It should have a default constructor.
#ifndef ECS_ALLOCATOR_TYPE
#define ECS_ALLOCATOR_TYPE std::allocator<ECS::Entity>
#endif

// Define ECS_TICK_NO_CLEANUP if you don't want the world to automatically cleanup dead entities
// at the beginning of each tick. This will require you to call cleanup() manually to prevent memory
// leaks.
//#define ECS_TICK_NO_CLEANUP

// Define ECS_NO_RTTI to turn off RTTI. This requires using the ECS_DEFINE_TYPE and ECS_DECLARE_TYPE macros on all types
// that you wish to use as components or events.
//#define ECS_NO_RTTI

#ifndef ECS_NO_RTTI
#include <typeindex>
#include <typeinfo>
#endif

//////////////////////////////////////////////////////////////////////////
// CODE //
//////////////////////////////////////////////////////////////////////////

namespace ECS
{
#ifndef ECS_NO_RTTI
	typedef std::type_index TypeIndex;

#define ECS_DECLARE_TYPE
#define ECS_DEFINE_TYPE(name)

	template<typename T>
	TypeIndex getTypeIndex()
	{
		return std::type_index(typeid(T));
	}

#else
	typedef uint32_t TypeIndex;

	namespace Internal
	{
		class TypeRegistry
		{
		public:
			TypeRegistry()
			{
				index = nextIndex;
				++nextIndex;
			}

			TypeIndex getIndex() const
			{
				return index;
			}

		private:
			static TypeIndex nextIndex;
			TypeIndex index;
		};

		TypeIndex TypeRegistry::nextIndex = 1;
	}

#define ECS_DECLARE_TYPE public: static ECS::Internal::TypeRegistry __ecs_type_reg
#define ECS_DEFINE_TYPE(name) ECS::Internal::TypeRegistry name::__ecs_type_reg

	template<typename T>
	TypeIndex getTypeIndex()
	{
		return T::__ecs_type_reg.getIndex();
	}
#endif

	class World;
	class Entity;

	typedef float DefaultTickData;
	typedef ECS_ALLOCATOR_TYPE Allocator;

	// Do not use anything in the Internal namespace yourself.
	namespace Internal
	{
		struct BaseComponentContainer
		{
			friend class Entity;

		protected:
			virtual void destroy(World* world) = 0;
		};

		template<typename T>
		struct ComponentContainer : public BaseComponentContainer
		{
			ComponentContainer() {}
			ComponentContainer(const T& data) : data(data) {}

			T data;

		protected:
			virtual void destroy(World* world)
			{
				using ComponentAllocator = std::allocator_traits<World::EntityAllocator>::template rebind_alloc<ComponentContainer<T>>;

				ComponentAllocator alloc(world->getPrimaryAllocator());
				std::allocator_traits<ComponentAllocator>::destroy(alloc, this);
				std::allocator_traits<ComponentAllocator>::deallocate(alloc, this, 1);
			}
		};


		class BaseEventSubscriber
		{
		public:
			virtual ~BaseEventSubscriber() {};
		};
	}

	/**
	* Think of this as a pointer to a component. Whenever you get a component from the world or an entity,
	* it'll be wrapped in a ComponentHandle.
	*/
	template<typename T>
	class ComponentHandle
	{
	public:
		ComponentHandle()
			: component(nullptr)
		{
		}

		ComponentHandle(T* component)
			: component(component)
		{
		}

		T* operator->() const
		{
			return component;
		}

		operator bool() const
		{
			return isValid();
		}

		T& get()
		{
			return *component;
		}

		bool isValid() const
		{
			return component != nullptr;
		}

	private:
		T* component;
	};

	/**
	* A container for components. Entities do not have any logic of their own, except of that which to manage
	* components. Components themselves are generally structs that contain data with which EntitySystems can
	* act upon, but technically any data type may be used as a component, though only one of each data type
	* may be on a single Entity at a time.
	*/
	class Entity
	{
	public:
		friend class World;

		const static size_t InvalidEntityId = 0;

		// Do not create entities yourself, use World::create().
		Entity(World* world, size_t id)
			: world(world), id(id)
		{
		}

		// Do not delete entities yourself, use World::destroy().
		~Entity()
		{
			removeAll();
		}

		/**
		* Get the world associated with this entity.
		*/
		World* getWorld() const
		{
			return world;
		}

		/**
		* Does this entity have a component?
		*/
		template<typename T>
		bool has() const
		{
			auto index = getTypeIndex<T>();
			return components.find(index) != components.end();
		}

		/**
		* Does this entity have this list of components? The order of components does not matter.
		*/
		template<typename T, typename V, typename... Types>
		bool has() const
		{
			return has<T>() && has<V, Types...>();
		}

		/**
		* Assign a new component (or replace an old one). All components must have a default constructor, though they
		* may have additional constructors. You may pass arguments to this function the same way you would to a constructor.
		*
		* It is recommended that components be simple types (not const, not references, not pointers). If you need to store
		* any of the above, wrap it in a struct.
		*/
		template<typename T, typename... Args>
		ComponentHandle<T> assign(Args&&... args)
		{
			using ComponentAllocator = std::allocator_traits<World::EntityAllocator>::template rebind_alloc<Internal::ComponentContainer<T>>;

			auto found = components.find(getTypeIndex<T>());
			if (found != components.end())
			{
				Internal::ComponentContainer<T>* container = reinterpret_cast<Internal::ComponentContainer<T>*>(found->second);
				container->data = T(args...);

				auto handle = ComponentHandle<T>(&container->data);
				world->emit<Events::OnComponentAssigned<T>>({ this, handle });
				return handle;
			}
			else
			{
				ComponentAllocator alloc(world->getPrimaryAllocator());

				Internal::ComponentContainer<T>* container = std::allocator_traits<ComponentAllocator>::allocate(alloc, 1);
				std::allocator_traits<ComponentAllocator>::construct(alloc, container, T(args...));

				components.insert({ getTypeIndex<T>(), container });

				auto handle = ComponentHandle<T>(&container->data);
				world->emit<Events::OnComponentAssigned<T>>({ this, handle });
				return handle;
			}
		}

		/**
		* Remove a component of a specific type. Returns whether a component was removed.
		*/
		template<typename T>
		bool remove()
		{
			auto found = components.find(getTypeIndex<T>());
			if (found != components.end())
			{
				found->second->destroy(world);
				components.erase(found);

				return true;
			}

			return false;
		}

		/**
		* Remove all components from this entity.
		*/
		void removeAll()
		{
			for (auto pair : components)
			{
				pair.second->destroy(world);
			}

			components.clear();
		}

		/**
		* Get a component from this entity.
		*/
		template<typename T>
		ComponentHandle<T> get()
		{
			auto found = components.find(getTypeIndex<T>());
			if (found != components.end())
			{
				return ComponentHandle<T>(&reinterpret_cast<Internal::ComponentContainer<T>*>(found->second)->data);
			}
			else
			{
				return ComponentHandle<T>(nullptr);
			}
		}

		/**
		* Call a function with components from this entity as arguments. This will return true if this entity has
		* all specified components attached, and false if otherwise.
		*/
		template<typename... Types>
		bool with(std::function<void(ComponentHandle<Types>...)> view)
		{
			if (!has<Types...>())
				return false;

			view(get<Types>()...); // variadic template expansion is fun
			return true;
		}

		/**
		* Get this entity's id. Entity ids aren't too useful at the moment, but can be used to tell the difference between entities when debugging.
		*/
		size_t getEntityId() const
		{
			return id;
		}

		bool isPendingDestroy() const
		{
			return bPendingDestroy;
		}

	private:
		std::unordered_map<TypeIndex, Internal::BaseComponentContainer*> components;
		World* world;

		size_t id;
		bool bPendingDestroy = false;
	};

	/**
	* A system that acts on entities. Generally, this will act on a subset of entities using World::each().
	*
	* Systems often will respond to events by subclassing EventSubscriber. You may use configure() to subscribe to events,
	* but remember to unsubscribe in unconfigure().
	*/
	class EntitySystem
	{
	public:
		virtual ~EntitySystem() {}

		/**
		* Called when this system is added to a world.
		*/
		virtual void configure(World* world)
		{
		}

		/**
		* Called when this system is being removed from a world.
		*/
		virtual void unconfigure(World* world)
		{
		}

		/**
		* Called when World::tick() is called. See ECS_TICK_TYPE at the top of this file for more
		* information about passing data to tick.
		*/
#ifdef ECS_TICK_TYPE_VOID
		virtual void tick(World* world)
#else
		virtual void tick(World* world, ECS_TICK_TYPE data)
#endif
		{
		}
	};

	/**
	* Subclass this as EventSubscriber<EventType> and then call World::subscribe() in order to subscribe to events. Make sure
	* to call World::unsubscribe() or World::unsubscribeAll() when your subscriber is deleted!
	*/
	template<typename T>
	class EventSubscriber : public Internal::BaseEventSubscriber
	{
	public:
		virtual ~EventSubscriber() {}

		/**
		* Called when an event is emitted by the world.
		*/
		virtual void receive(World* world, const T& event) = 0;
	};

	namespace Events
	{
		// Called when a new entity is created.
		struct OnEntityCreated
		{
			ECS_DECLARE_TYPE;

			Entity* entity;
		};

		ECS_DEFINE_TYPE(OnEntityCreated);

		// Called when an entity is about to be destroyed.
		struct OnEntityDestroyed
		{
			ECS_DECLARE_TYPE;

			Entity* entity;
		};

		ECS_DEFINE_TYPE(OnEntityDestroyed);

		// Called when a component is assigned (not necessarily created).
		template<typename T>
		struct OnComponentAssigned
		{
			ECS_DECLARE_TYPE;

			Entity* entity;
			ComponentHandle<T> component;
		};

#ifdef ECS_NO_RTTI
		template<typename T>
		ECS_DEFINE_TYPE(OnComponentAssigned<T>);
#endif
	}

	namespace Internal
	{
		class EntityIterator
		{
		public:
			EntityIterator(World* world, size_t index, bool bIsEnd, bool bIncludePendingDestroy);

			size_t getIndex() const
			{
				return index;
			}

			bool isEnd() const;

			bool includePendingDestroy() const
			{
				return bIncludePendingDestroy;
			}

			World* getWorld() const
			{
				return world;
			}

			Entity* get() const;

			Entity* operator*() const
			{
				return get();
			}

			bool operator==(const EntityIterator& other) const
			{
				if (world != other.world)
					return false;

				if (isEnd())
					return other.isEnd();

				return index == other.index;
			}

			bool operator!=(const EntityIterator& other) const
			{
				if (world != other.world)
					return true;

				if (isEnd())
					return !other.isEnd();

				return index != other.index;
			}

			EntityIterator& operator++();

		private:
			bool bIsEnd = false;
			size_t index;
			class ECS::World* world;
			bool bIncludePendingDestroy;
		};

		class EntityView
		{
		public:
			EntityView(const EntityIterator& first, const EntityIterator& last)
				: firstItr(first), lastItr(last)
			{
				if (firstItr.get() == nullptr || (firstItr.get()->isPendingDestroy() && !firstItr.includePendingDestroy()))
				{
					++firstItr;
				}
			}

			EntityIterator begin()
			{
				return firstItr;
			}

			EntityIterator end()
			{
				return lastItr;
			}

		private:
			EntityIterator firstItr;
			EntityIterator lastItr;
		};

		template<typename... Types>
		class EntityComponentIterator
		{
		public:
			EntityComponentIterator(World* world, size_t index, bool bIsEnd, bool bIncludePendingDestroy)
				: bIsEnd(bIsEnd), index(index), world(world), bIncludePendingDestroy(bIncludePendingDestroy)
			{
				if (index >= world->getCount())
					this->bIsEnd = true;
			}

			size_t getIndex() const
			{
				return index;
			}

			bool isEnd() const
			{
				return bIsEnd || index >= world->getCount();
			}

			bool includePendingDestroy() const
			{
				return bIncludePendingDestroy;
			}

			World* getWorld() const
			{
				return world;
			}

			Entity* get() const
			{
				if (isEnd())
					return nullptr;

				return world->getByIndex(index);
			}

			Entity* operator*() const
			{
				return get();
			}

			bool operator==(const EntityComponentIterator<Types...>& other) const
			{
				if (world != other.world)
					return false;

				if (isEnd())
					return other.isEnd();

				return index == other.index;
			}

			bool operator!=(const EntityComponentIterator<Types...>& other) const
			{
				if (world != other.world)
					return true;

				if (isEnd())
					return !other.isEnd();

				return index != other.index;
			}

			EntityComponentIterator<Types...>& operator++()
			{
				++index;
				while (index < world->getCount() && (get() == nullptr || !get()->has<Types...>() || (get()->isPendingDestroy() && !bIncludePendingDestroy)))
				{
					++index;
				}

				if (index >= world->getCount())
					bIsEnd = true;

				return *this;
			}

		private:
			bool bIsEnd = false;
			size_t index;
			class ECS::World* world;
			bool bIncludePendingDestroy;
		};

		template<typename... Types>
		class EntityComponentView
		{
		public:
			EntityComponentView(const EntityComponentIterator<Types...>& first, const EntityComponentIterator<Types...>& last)
				: firstItr(first), lastItr(last)
			{
				if (firstItr.get() == nullptr || (firstItr.get()->isPendingDestroy() && !firstItr.includePendingDestroy())
					|| !firstItr.get()->has<Types...>())
				{
					++firstItr;
				}
			}

			EntityComponentIterator<Types...> begin()
			{
				return firstItr;
			}

			EntityComponentIterator<Types...> end()
			{
				return lastItr;
			}

		private:
			EntityComponentIterator<Types...> firstItr;
			EntityComponentIterator<Types...> lastItr;
		};
	}

	/**
	* The world creates, destroys, and manages entities. The lifetime of entities and _registered_ systems are handled by the world
	* (don't delete a system without unregistering it from the world first!), while event subscribers have their own lifetimes
	* (the world doesn't delete them automatically when the world is deleted).
	*/
	class World
	{
	public:
		using WorldAllocator = std::allocator_traits<Allocator>::template rebind_alloc<World>;
		using EntityAllocator = std::allocator_traits<Allocator>::template rebind_alloc<Entity>;
		using SystemAllocator = std::allocator_traits<Allocator>::template rebind_alloc<EntitySystem>;
		using EntityPtrAllocator = std::allocator_traits<Allocator>::template rebind_alloc<Entity*>;
		using SystemPtrAllocator = std::allocator_traits<Allocator>::template rebind_alloc<EntitySystem*>;
		using SubscriberPtrAllocator = std::allocator_traits<Allocator>::template rebind_alloc<Internal::BaseEventSubscriber*>;
		using SubscriberPairAllocator = std::allocator_traits<Allocator>::template rebind_alloc<std::pair<const TypeIndex, std::vector<Internal::BaseEventSubscriber*, SubscriberPtrAllocator>>>;

		/**
		* Use this function to construct the world with a custom allocator.
		*/
		static World* createWorld(Allocator alloc)
		{
			WorldAllocator worldAlloc(alloc);
			World* world = std::allocator_traits<WorldAllocator>::allocate(worldAlloc, 1);
			std::allocator_traits<WorldAllocator>::construct(worldAlloc, world, alloc);

			return world;
		}

		/**
		* Use this function to construct the world with the default allocator.
		*/
		static World* createWorld()
		{
			return createWorld(Allocator());
		}

		// Use this to destroy the world instead of calling delete.
		// This will emit OnEntityDestroyed events and call EntitySystem::unconfigure as appropriate.
		void destroyWorld()
		{
			WorldAllocator alloc(entAlloc);
			std::allocator_traits<WorldAllocator>::destroy(alloc, this);
			std::allocator_traits<WorldAllocator>::deallocate(alloc, this, 1);
		}

		World(Allocator alloc)
			: entAlloc(alloc), systemAlloc(alloc),
			entities({}, EntityPtrAllocator(alloc)),
			systems({}, SystemPtrAllocator(alloc)),
			subscribers({}, 0, std::hash<TypeIndex>(), std::equal_to<TypeIndex>(), SubscriberPtrAllocator(alloc))
		{
		}

		/**
		* Destroying the world will emit OnEntityDestroyed events and call EntitySystem::unconfigure() as appropriate.
		*
		* Use World::destroyWorld to destroy and deallocate the world, do not manually delete the world!
		*/
		~World()
		{
			for (auto* ent : entities)
			{
				if (!ent->isPendingDestroy())
				{
					ent->bPendingDestroy = true;
					emit<Events::OnEntityDestroyed>({ ent });
				}

				std::allocator_traits<EntityAllocator>::destroy(entAlloc, ent);
				std::allocator_traits<EntityAllocator>::deallocate(entAlloc, ent, 1);
			}

			for (auto* system : systems)
			{
				system->unconfigure(this);
				std::allocator_traits<SystemAllocator>::destroy(systemAlloc, system);
				std::allocator_traits<SystemAllocator>::deallocate(systemAlloc, system, 1);
			}
		}

		/**
		* Create a new entity. This will emit the OnEntityCreated event.
		*/
		Entity* create()
		{
			++lastEntityId;
			Entity* ent = std::allocator_traits<EntityAllocator>::allocate(entAlloc, 1);
			std::allocator_traits<EntityAllocator>::construct(entAlloc, ent, this, lastEntityId);
			entities.push_back(ent);

			emit<Events::OnEntityCreated>({ ent });

			return ent;
		}

		/**
		* Destroy an entity. This will emit the OnEntityDestroy event.
		*
		* If immediate is false (recommended), then the entity won't be immediately
		* deleted but instead will be removed at the beginning of the next tick() or
		* when cleanup() is called. OnEntityDestroyed will still be called immediately.
		*
		* This function is safe to call multiple times on a single entity. Note that calling
		* this once with immediate = false and then calling it with immediate = true will
		* remove the entity from the pending destroy queue and will immediately destroy it
		* _without_ emitting a second OnEntityDestroyed event.
		*
		* A warning: Do not set immediate to true if you are currently iterating through entities!
		*/
		void destroy(Entity* ent, bool immediate = false)
		{
			if (ent == nullptr)
				return;

			if (ent->isPendingDestroy())
			{
				if (immediate)
				{
					entities.erase(std::remove(entities.begin(), entities.end(), ent), entities.end());
					std::allocator_traits<EntityAllocator>::destroy(entAlloc, ent);
					std::allocator_traits<EntityAllocator>::deallocate(entAlloc, ent, 1);
				}

				return;
			}

			ent->bPendingDestroy = true;

			emit<Events::OnEntityDestroyed>({ ent });

			if (immediate)
			{
				entities.erase(std::remove(entities.begin(), entities.end(), ent), entities.end());
				std::allocator_traits<EntityAllocator>::destroy(entAlloc, ent);
				std::allocator_traits<EntityAllocator>::deallocate(entAlloc, ent, 1);
			}
		}

		/**
		* Delete all entities in the pending destroy queue. Returns true if any entities were cleaned up,
		* false if there were no entities to clean up.
		*/
		bool cleanup()
		{
			size_t count = 0;
			entities.erase(std::remove_if(entities.begin(), entities.end(), [&, this](auto* ent) {
				if (ent->isPendingDestroy())
				{
					std::allocator_traits<EntityAllocator>::destroy(entAlloc, ent);
					std::allocator_traits<EntityAllocator>::deallocate(entAlloc, ent, 1);
					++count;
					return true;
				}

				return false;
			}), entities.end());

			return count > 0;
		}

		/**
		* Reset the world, destroying all entities. Entity ids will be reset as well.
		*/
		void reset()
		{
			for (auto* ent : entities)
			{
				if (!ent->isPendingDestroy())
				{
					ent->bPendingDestroy = true;
					emit<Events::OnEntityDestroyed>({ ent });
				}
				std::allocator_traits<EntityAllocator>::destroy(entAlloc, ent);
				std::allocator_traits<EntityAllocator>::deallocate(entAlloc, ent, 1);
			}

			entities.clear();
			lastEntityId = 0;
		}

		/**
		* Register a system. The world will manage the memory of the system unless you unregister the system.
		*/
		void registerSystem(EntitySystem* system)
		{
			systems.push_back(system);
			system->configure(this);
		}

		/**
		* Unregister a system.
		*/
		void unregisterSystem(EntitySystem* system)
		{
			systems.erase(std::remove(systems.begin(), systems.end(), system), systems.end());
			system->unconfigure(this);
		}

		/**
		* Subscribe to an event.
		*/
		template<typename T>
		void subscribe(EventSubscriber<T>* subscriber)
		{
			auto index = getTypeIndex<T>();
			auto found = subscribers.find(index);
			if (found == subscribers.end())
			{
				std::vector<Internal::BaseEventSubscriber*, SubscriberPtrAllocator> subList(entAlloc);
				subList.push_back(subscriber);

				subscribers.insert({ index, subList });
			}
			else
			{
				found->second.push_back(subscriber);
			}
		}

		/**
		* Unsubscribe from an event.
		*/
		template<typename T>
		void unsubscribe(EventSubscriber<T>* subscriber)
		{
			auto index = getTypeIndex<T>();
			if (found != subscribers.end())
			{
				found->second.erase(std::remove(found->second.begin(), found->second.end(), subscriber), found->second.end());
				if (found->second.size() == 0)
				{
					subscribers.erase(found);
				}
			}
		}

		/**
		* Unsubscribe from all events. Don't be afraid of the void pointer, just pass in your subscriber as normal.
		*/
		void unsubscribeAll(void* subscriber)
		{
			for (auto kv : subscribers)
			{
				kv.second.erase(std::remove(kv.second.begin(), kv.second.end(), subscriber), kv.second.end());
				if (kv.second.size() == 0)
				{
					subscribers.erase(kv.first);
				}
			}
		}

		/**
		* Emit an event. This will do nothing if there are no subscribers for the event type.
		*/
		template<typename T>
		void emit(const T& event)
		{
			auto found = subscribers.find(getTypeIndex<T>());
			if (found != subscribers.end())
			{
				for (auto* base : found->second)
				{
					auto* sub = reinterpret_cast<EventSubscriber<T>*>(base);
					sub->receive(this, event);
				}
			}
		}

		/**
		* Run a function on each entity with a specific set of components. This is useful for implementing an EntitySystem.
		*
		* If you want to include entities that are pending destruction, set includePendingDestroy to true.
		*/
		template<typename... Types>
		void each(std::function<void(Entity*, ComponentHandle<Types>...)> viewFunc, bool bIncludePendingDestroy = false)
		{
			for (auto* ent : each<Types...>(bIncludePendingDestroy))
			{
				viewFunc(ent, ent->get<Types>()...);
			}
		}

		/**
		* Run a function on all entities.
		*/
		void all(std::function<void(Entity*)> viewFunc, bool bIncludePendingDestroy = false)
		{
			for (auto* ent : all(bIncludePendingDestroy))
			{
				viewFunc(ent);
			}
		}

		/**
		* Get a view for entities with a specific set of components. The list of entities is calculated on the fly, so this method itself
		* has little overhead. This is mostly useful with a range based for loop.
		*/
		template<typename... Types>
		Internal::EntityComponentView<Types...> each(bool bIncludePendingDestroy = false)
		{
			Internal::EntityComponentIterator<Types...> first(this, 0, false, bIncludePendingDestroy);
			Internal::EntityComponentIterator<Types...> last(this, getCount(), true, bIncludePendingDestroy);
			return Internal::EntityComponentView<Types...>(first, last);
		}

		Internal::EntityView all(bool bIncludePendingDestroy = false)
		{
			Internal::EntityIterator first(this, 0, false, bIncludePendingDestroy);
			Internal::EntityIterator last(this, getCount(), true, bIncludePendingDestroy);
			return Internal::EntityView(first, last);
		}

		size_t getCount() const
		{
			return entities.size();
		}

		Entity* getByIndex(size_t idx)
		{
			if (idx >= getCount())
				return nullptr;

			return entities[idx];
		}

		/**
		* Get an entity by an id. This is a slow process.
		*/
		Entity* getById(size_t id) const
		{
			if (id == Entity::InvalidEntityId || id > lastEntityId)
				return nullptr;

			// We should likely store entities in a map of id -> entity so that this is faster.
			for (auto* ent : entities)
			{
				if (ent->getEntityId() == id)
					return ent;
			}

			return nullptr;
		}

		/**
		* Tick the world. See the definition for ECS_TICK_TYPE at the top of this file for more information on
		* passing data through tick().
		*/
#ifdef ECS_TICK_TYPE_VOID
		void tick()
#else
		void tick(ECS_TICK_TYPE data)
#endif
		{
#ifndef ECS_TICK_NO_CLEANUP
			cleanup();
#endif
			for (auto* system : systems)
			{
#ifdef ECS_TICK_TYPE_VOID
				system->tick(this);
#else
				system->tick(this, data);
#endif
			}
		}

		EntityAllocator& getPrimaryAllocator()
		{
			return entAlloc;
		}

	private:
		EntityAllocator entAlloc;
		SystemAllocator systemAlloc;

		std::vector<Entity*, EntityPtrAllocator> entities;
		std::vector<EntitySystem*, SystemPtrAllocator> systems;
		std::unordered_map<TypeIndex,
			std::vector<Internal::BaseEventSubscriber*, SubscriberPtrAllocator>,
			std::hash<TypeIndex>,
			std::equal_to<TypeIndex>,
			SubscriberPairAllocator> subscribers;

		size_t lastEntityId = 0;
	};

	namespace Internal
	{
		inline EntityIterator::EntityIterator(class World* world, size_t index, bool bIsEnd, bool bIncludePendingDestroy)
			: bIsEnd(bIsEnd), index(index), world(world), bIncludePendingDestroy(bIncludePendingDestroy)
		{
			if (index >= world->getCount())
				this->bIsEnd = true;
		}

		inline bool EntityIterator::isEnd() const
		{
			return bIsEnd || index >= world->getCount();
		}

		inline Entity* EntityIterator::get() const
		{
			if (isEnd())
				return nullptr;

			return world->getByIndex(index);
		}

		inline EntityIterator& EntityIterator::operator++()
		{
			++index;
			while (index < world->getCount() && (get() == nullptr || (get()->isPendingDestroy() && !bIncludePendingDestroy)))
			{
				++index;
			}

			if (index >= world->getCount())
				bIsEnd = true;

			return *this;
		}
	}
}