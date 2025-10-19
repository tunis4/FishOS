#include <dev/devnode.hpp>
#include <dev/simple_char.hpp>
#include <dev/tty/console.hpp>
#include <dev/tty/pty.hpp>
#include <dev/tty/tty.hpp>
#include <dev/input/evdev.hpp>
#include <klib/vector.hpp>
#include <gfx/framebuffer.hpp>

namespace dev {
    static klib::Vector<CharDevNode::NodeInitializer>& get_char_device_initializers() {
        static klib::Vector<CharDevNode::NodeInitializer> char_device_initializers;
        return char_device_initializers;
    }

    static klib::Vector<BlockDevNode::NodeInitializer>& get_block_device_initializers() {
        static klib::Vector<BlockDevNode::NodeInitializer> block_device_initializers;
        return block_device_initializers;
    }

    void CharDevNode::register_node_initializer(uint major, uint minor, const char *name, klib::Function<CharDevNode*()> &&create) {
        get_char_device_initializers().emplace_back(make_dev_id(major, minor), name, klib::move(create));
    }

    CharDevNode* CharDevNode::create_node(dev_t dev_id) {
        for (NodeInitializer &initializer : get_char_device_initializers()) {
            if (initializer.dev_id == dev_id) {
                CharDevNode *device = initializer.create();
                device->dev_id = dev_id;
                return device;
            }
        }
        return nullptr;
    }

    void BlockDevNode::register_node_initializer(uint major, uint minor, const char *name, klib::Function<BlockDevNode*()> &&create) {
        get_block_device_initializers().emplace_back(make_dev_id(major, minor), name, klib::move(create));
    }

    BlockDevNode* BlockDevNode::create_node(dev_t dev_id) {
        for (NodeInitializer &initializer : get_block_device_initializers()) {
            if (initializer.dev_id == dev_id) {
                BlockDevNode *device = initializer.create();
                device->dev_id = dev_id;
                return device;
            }
        }
        return nullptr;
    }

    isize BlockDevNode::read(vfs::FileDescription *fd, void *buf, usize count, usize offset) {
        return klib::sync(block_device->read_write(buf, count, offset, READ));
    }

    isize BlockDevNode::write(vfs::FileDescription *fd, const void *buf, usize count, usize offset) {
        return klib::sync(block_device->read_write((void*)buf, count, offset, WRITE));
    }

    void init_devices() {
        CharDevNode::register_node_initializer( 1,  1,     "mem", [] { return new MemDevNode(); });
        CharDevNode::register_node_initializer( 1,  3,    "null", [] { return new NullDevNode(); });
        CharDevNode::register_node_initializer( 1,  4,    "port", [] { return new PortDevNode(); });
        CharDevNode::register_node_initializer( 1,  5,    "zero", [] { return new ZeroDevNode(); });
        CharDevNode::register_node_initializer( 1,  7,    "full", [] { return new FullDevNode(); });
        CharDevNode::register_node_initializer( 1,  8,  "random", [] { return new RandomDevNode(); });
        CharDevNode::register_node_initializer( 1,  9, "urandom", [] { return new URandomDevNode(); });
        CharDevNode::register_node_initializer( 5,  0,     "tty", [] { return new tty::TTYDevNode(); });
        CharDevNode::register_node_initializer( 5,  1, "console", [] { return new tty::ConsoleDevNode(); });
        CharDevNode::register_node_initializer( 5,  2,    "ptmx", [] { return new tty::PseudoTerminalMultiplexer(); });
        CharDevNode::register_node_initializer(29,  0,     "fb0", [] { return new gfx::FramebufferDevNode(); });
        CharDevNode::register_node_initializer(13, 64,  "event0", [] { return new input::EventDevNode(input::main_keyboard); });
        CharDevNode::register_node_initializer(13, 65,  "event1", [] { return new input::EventDevNode(input::main_mouse); });
    }
}
