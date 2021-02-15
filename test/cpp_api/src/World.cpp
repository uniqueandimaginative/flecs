#include <cpp_api.h>

static
void install_test_abort() {
    ecs_os_set_api_defaults();
    ecs_os_api_t os_api = ecs_os_api;
    os_api.abort_ = test_abort;
    ecs_os_set_api(&os_api);
    ecs_tracing_enable(-2);
}

void World_multi_world_empty() {
    flecs::world *w1 = new flecs::world();
    delete w1;
    flecs::world *w2 = new flecs::world();
    delete w2;

    test_assert(true);
}

class FooModule {
public:
    FooModule(flecs::world& world) { 
        world.module<FooModule>();
    }
};

typedef struct TestInteropModule {
    int dummy;
} TestInteropModule;

static
void TestInteropModuleImport(ecs_world_t *world) {
    ECS_MODULE(world, TestInteropModule);

    ECS_COMPONENT(world, Position);
}

namespace test {
namespace interop {

class module : TestInteropModule {
public:
    module(flecs::world& ecs) {
        TestInteropModuleImport(ecs.c_ptr());

        ecs.module<test::interop::module>();

        ecs.component<Position>("test::interop::module::Position");
    }
};

}
}

void World_multi_world_component() {
    flecs::world w1;
    flecs::world w2;

    auto p_1 = w1.component<Position>();
    auto v_1 = w1.component<Velocity>();
    auto v_2 = w2.component<Velocity>();
    auto m_2 = w2.component<Mass>();

    test_assert(v_1.id() == v_2.id());
    test_assert(p_1.id() != m_2.id());
    test_assert(m_2.id() > v_2.id());

    auto p_2 = w2.component<Position>();
    test_assert(p_1.id() == p_2.id());
}

namespace A {
    struct Comp {
        float x;
        float y;
    };
}

void World_multi_world_component_namespace() {
    flecs::world *w = new flecs::world();
    auto c = w->component<A::Comp>();
    auto id_1 = c.id();
    delete w;

    w = new flecs::world();
    c = w->component<A::Comp>();
    auto id_2 = c.id();

    test_assert(id_1 == id_2);
}

void World_type_id() {
    flecs::world w;

    auto p = w.component<Position>();

    test_assert(p.id() == flecs::type_id<Position>());
}

void World_different_comp_same_name() {
    flecs::world w;

    install_test_abort();
    test_expect_abort();

    w.component<Position>("Position");
    w.component<Velocity>("Position");
}

void World_reregister_after_reset() {
    flecs::world w;

    auto p1 = w.component<Position>("Position");

    // Simulate different binary
    flecs::_::component_info<Position>::reset();

    auto p2 = w.component<Position>("Position");

    test_assert(p1.id() == p2.id());
}

void World_implicit_reregister_after_reset() {
    flecs::world w;

    w.entity().add<Position>();

    flecs::entity_t p_id_1 = flecs::type_id<Position>();

    // Simulate different binary
    flecs::_::component_info<Position>::reset();

    w.entity().add<Position>();

    flecs::entity_t p_id_2 = flecs::type_id<Position>();

    test_assert(p_id_1 == p_id_2);
}

void World_reregister_after_reset_different_name() {
    flecs::world w;

    install_test_abort();
    test_expect_abort();    

    w.component<Position>("Position");

    // Simulate different binary
    flecs::_::component_info<Position>::reset();

    w.component<Position>("Velocity");
}

void World_reimport_module_after_reset() {
    flecs::world w;

    auto m1 = w.import<FooModule>();

    // Simulate different binary
    flecs::_::component_info<FooModule>::reset();

    auto m2 = w.import<FooModule>();
    
    test_assert(m1.id() == m2.id());
}

void World_reimport_module_new_world() {
    flecs::entity e1;
    {
        flecs::world w;

        e1 = w.import<FooModule>();
    }

    {
        flecs::world w;

        auto e2 = w.import<FooModule>();
        
        test_assert(e1.id() == e2.id());
    }
}

void World_c_interop_module() {
    flecs::world w;

    w.import<test::interop::module>();


}
