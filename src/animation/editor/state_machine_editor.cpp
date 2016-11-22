#include "state_machine_editor.h"
#include "animation/animation.h"
#include "animation/controller.h"
#include "animation/editor/animation_editor.h"
#include "animation/events.h"
#include "animation/state_machine.h"
#include "engine/blob.h"
#include "engine/crc32.h"
#include "engine/engine.h"
#include "engine/resource_manager.h"
#include "engine/resource_manager_base.h"
#include <cmath>


using namespace Lumix;


static const ResourceType CONTROLLER_RESOURCE_TYPE("anim_controller");


static ImVec2 operator+(const ImVec2& a, const ImVec2& b)
{
	return ImVec2(a.x + b.x, a.y + b.y);
}


static ImVec2 operator-(const ImVec2& a, const ImVec2& b)
{
	return ImVec2(a.x - b.x, a.y - b.y);
}


static ImVec2 operator*(const ImVec2& a, float b)
{
	return ImVec2(a.x * b, a.y * b);
}


static float dot(const ImVec2& a, const ImVec2& b)
{
	return a.x * b.x + a.y * b.y;
}


namespace AnimEditor
{


static ImVec2 getEdgeStartPoint(const ImVec2 a_pos, const ImVec2 a_size, const ImVec2 b_pos, const ImVec2 b_size, bool is_dir)
{
	ImVec2 center_a = a_pos + a_size * 0.5f;
	ImVec2 center_b = b_pos + b_size * 0.5f;
	ImVec2 dir = center_b - center_a;
	if (fabs(dir.x / dir.y) > fabs(a_size.x / a_size.y))
	{
		dir = dir * fabs(1 / dir.x);
		return center_a + dir * a_size.x * 0.5f + ImVec2(0, center_a.y > center_b.y == is_dir ? 5.0f : -5.0f);
	}

	dir = dir * fabs(1 / dir.y);
	return center_a + dir * a_size.y * 0.5f + ImVec2(center_a.x > center_b.x == is_dir ? 5.0f : -5.0f, 0);
}



static ImVec2 getEdgeStartPoint(Node* a, Node* b, bool is_dir)
{
	return getEdgeStartPoint(a->pos, a->size, b->pos, b->size, is_dir);
}


Component::~Component()
{
	if (getParent())
	{
		getParent()->removeChild(this);
	}
}


Node::Node(Anim::Component* engine_cmp, Container* parent, ControllerResource& controller)
	: Component(engine_cmp, parent, controller)
	, m_edges(controller.getAllocator())
	, m_in_edges(controller.getAllocator())
	, m_allocator(controller.getAllocator())
{
	m_name = "";
}


Node::~Node()
{
	while (!m_edges.empty())
	{
		LUMIX_DELETE(m_controller.getAllocator(), m_edges.back());
	}
	while (!m_in_edges.empty())
	{
		LUMIX_DELETE(m_controller.getAllocator(), m_in_edges.back());
	}
}


bool Node::hitTest(const ImVec2& on_canvas_pos) const
{
	return on_canvas_pos.x >= pos.x && on_canvas_pos.x < pos.x + size.x
		&& on_canvas_pos.y >= pos.y && on_canvas_pos.y < pos.y + size.y;
}


void Node::removeEvent(int index)
{
	auto* engine_node = ((Anim::Node*)engine_cmp);
	auto& events = engine_node->events;
	Anim::EventHeader header = *(Anim::EventHeader*)&events[sizeof(Anim::EventHeader) * index];
	u8* headers_end = &events[sizeof(Anim::EventHeader) * engine_node->events_count];
	u8* end = &events.back() + 1;
	u8* event_start = headers_end + header.offset;
	u8* event_end = event_start + header.size;
	
	u8* header_start = &events[sizeof(Anim::EventHeader) * index];
	u8* header_end = header_start + sizeof(Anim::EventHeader);
	moveMemory(header_start, header_end, event_start - header_end);
	moveMemory(event_start - sizeof(Anim::EventHeader), event_end, end - event_end);
	
	--engine_node->events_count;
}


void Node::onGUI()
{
	ImGui::InputText("Name", m_name.data, lengthOf(m_name.data));
	if (engine_cmp && ImGui::CollapsingHeader("Events"))
	{
		auto* engine_node = ((Anim::Node*)engine_cmp);
		auto& events = engine_node->events;
		for(int i = 0; i < engine_node->events_count; ++i)
		{
			if (ImGui::TreeNode((void*)(intptr_t)i, "%d", i))
			{
				Anim::EventHeader& header = *(Anim::EventHeader*)&events[sizeof(Anim::EventHeader) * i];
				if (ImGui::Button("Remove"))
				{
					removeEvent(i);
					ImGui::TreePop();
					break;
				}
				ImGui::InputFloat("Time", &header.time);
				switch (header.type)
				{
					case Anim::EventHeader::SET_INPUT:
					{
						int event_offset = header.offset + sizeof(Anim::EventHeader) * engine_node->events_count;
						auto event = (Anim::SetInputEvent*)&events[event_offset];
						auto& input_decl = m_controller.getEngineResource()->getInputDecl();
						auto getter = [](void* data, int idx, const char** out) -> bool {
							auto& input_decl = *(Anim::InputDecl*)data;
							*out = input_decl.inputs[idx].name;
							return true;
						};
						ImGui::Combo("Input", &event->input_idx, getter, &input_decl, input_decl.inputs_count);
						if (event->input_idx >= 0 && event->input_idx < input_decl.inputs_count)
						{
							switch (input_decl.inputs[event->input_idx].type)
							{
								case Anim::InputDecl::BOOL: ImGui::Checkbox("Value", &event->b_value); break;
								case Anim::InputDecl::INT: ImGui::InputInt("Value", &event->i_value); break;
								case Anim::InputDecl::FLOAT: ImGui::InputFloat("Value", &event->f_value); break;
								default: ASSERT(false); break;
							}
						}
					}
					break;
					default: ASSERT(false); break;
				}
				ImGui::TreePop();
			}
		}

		static int current = 0;
		ImGui::Combo("", &current, "Set Input\0");
		ImGui::SameLine();
		if (ImGui::Button("Add event"))
		{
			auto newEvent = [&](int size, u8 type) {
				int old_payload_size = events.size() - sizeof(Anim::EventHeader) * engine_node->events_count;
				events.resize(events.size() + size + sizeof(Anim::EventHeader));
				u8* headers_end = &events[engine_node->events_count * sizeof(Anim::EventHeader)];
				moveMemory(headers_end, headers_end + sizeof(Anim::EventHeader), old_payload_size);
				Anim::EventHeader& event_header =
					*(Anim::EventHeader*)&events[sizeof(Anim::EventHeader) * engine_node->events_count];
				event_header.type = type;
				event_header.time = 0;
				event_header.size = size;
				event_header.offset = old_payload_size;
				return headers_end + old_payload_size;
			};

			switch (current)
			{
				case Anim::EventHeader::SET_INPUT: newEvent((int)sizeof(Anim::SetInputEvent), Anim::EventHeader::SET_INPUT); break;
				default: ASSERT(false); break;
			}
			++engine_node->events_count;
		}
	}
}


void Node::serialize(OutputBlob& blob)
{
	blob.write(pos);
	blob.write(size);
	blob.write(m_name);
}


void Node::deserialize(InputBlob& blob)
{
	blob.read(pos);
	blob.read(size);
	blob.read(m_name);
}


static ImVec2 drawNode(ImDrawList* draw, const char* label, const ImVec2 pos, bool selected)
{
	float text_width = ImGui::CalcTextSize(label).x;
	ImVec2 size;
	size.x = Math::maximum(50.0f, text_width + ImGui::GetStyle().FramePadding.x * 2);
	size.y = ImGui::GetTextLineHeightWithSpacing() * 2;
	ImVec2 from = pos;
	ImVec2 to = from + size;
	ImU32 color = ImGui::ColorConvertFloat4ToU32(
		selected ? ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered] : ImGui::GetStyle().Colors[ImGuiCol_Button]);

	draw->AddRectFilled(from, to, color, 5);
	draw->AddRect(from + ImVec2(1, 1), to + ImVec2(1, 1), ImGui::GetColorU32(ImGuiCol_BorderShadow), 5);
	draw->AddRect(from, to, ImGui::GetColorU32(ImGuiCol_Border), 5);

	ImGui::SetCursorScreenPos(from + ImVec2((size.x - text_width) * 0.5f, size.y * 0.25f));
	ImGui::Text("%s", label);

	ImGui::SetCursorScreenPos(from);
	ImGui::InvisibleButton("bg", size);
	return size;

}


bool Node::draw(ImDrawList* draw, const ImVec2& canvas_screen_pos, bool selected)
{
	ImGui::PushID(engine_cmp);
	size = drawNode(draw, m_name, canvas_screen_pos + pos, selected);
	ImGui::PopID();
	return ImGui::IsItemActive();
}


Container::Container(Anim::Component* engine_cmp, Container* parent, ControllerResource& controller)
	: Node(engine_cmp, parent, controller)
	, m_editor_cmps(controller.getAllocator())
	, m_selected_component(nullptr)
{
}


Container::~Container()
{
	while (!m_editor_cmps.empty())
	{
		LUMIX_DELETE(m_controller.getAllocator(), m_editor_cmps.back());
	}
}


void Container::removeChild(Component* component)
{
	auto* engine_container = ((Anim::Container*)engine_cmp);
	engine_container->children.eraseItem(component->engine_cmp);
	m_editor_cmps.eraseItem(component);
	if (component == m_selected_component) m_selected_component = nullptr;
}


Component* Container::childrenHitTest(const ImVec2& pos)
{
	for (auto* i : m_editor_cmps)
	{
		if (i->hitTest(pos)) return i;
	}
	return nullptr;
}


Component* Container::getChildByUID(int uid)
{
	for (auto* i : m_editor_cmps)
	{
		if (i->engine_cmp && i->engine_cmp->uid == uid) return i;
	}
	return nullptr;
}


Edge::Edge(Anim::Edge* engine_cmp, Container* parent, ControllerResource& controller)
	: Component(engine_cmp, parent, controller)
{
	m_from = (Node*)parent->getChildByUID(engine_cmp->from->uid);
	m_to = (Node*)parent->getChildByUID(engine_cmp->to->uid);
	ASSERT(m_from);
	ASSERT(m_to);
	m_from->addEdge(this);
	m_to->addInEdge(this);
	m_expression[0] = 0;
}


Edge::~Edge()
{
	m_from->removeEdge(this);
	m_to->removeInEdge(this);
}


void Edge::debug(ImDrawList* draw, const ImVec2& canvas_screen_pos, Lumix::Anim::ComponentInstance* runtime)
{
	if (runtime->source.type != engine_cmp->type) return;
	
	ImVec2 from = getEdgeStartPoint(m_from, m_to, true) + canvas_screen_pos;
	ImVec2 to = getEdgeStartPoint(m_to, m_from, false) + canvas_screen_pos;

	float t = runtime->getTime() / runtime->getLength();
	ImVec2 p = from + (to - from) * t;
	ImVec2 dir = to - from;
	dir = dir * (1 / sqrt(dot(dir, dir))) * 2;
	draw->AddLine(p - dir, p + dir, 0xfff00FFF, 3);
}


void Edge::compile()
{
	auto* engine_edge = (Anim::Edge*)engine_cmp;
	engine_edge->condition.compile(m_expression, m_controller.getEngineResource()->getInputDecl());
}


void Edge::onGUI()
{
	auto* engine_edge = (Anim::Edge*)engine_cmp;
	ImGui::DragFloat("Length", &engine_edge->length);
	if (ImGui::InputText("Expression", m_expression, lengthOf(m_expression), ImGuiInputTextFlags_EnterReturnsTrue))
	{
		engine_edge->condition.compile(m_expression, m_controller.getEngineResource()->getInputDecl());
	}
}


bool Edge::draw(ImDrawList* draw, const ImVec2& canvas_screen_pos, bool selected)
{
	u32 color = ImGui::ColorConvertFloat4ToU32(
		selected ? ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered] : ImGui::GetStyle().Colors[ImGuiCol_Button]);
	ImVec2 from = getEdgeStartPoint(m_from, m_to, true) + canvas_screen_pos;
	ImVec2 to = getEdgeStartPoint(m_to, m_from, false) + canvas_screen_pos;
	draw->AddLine(from, to, color);
	ImVec2 dir = to - from;
	dir = dir * (1 / sqrt(dot(dir, dir))) * 5;
	ImVec2 right(dir.y, -dir.x);
	draw->AddLine(to, to - dir + right, color);
	draw->AddLine(to, to - dir - right, color);
	if (ImGui::IsMouseClicked(0) && hitTest(ImGui::GetMousePos() - canvas_screen_pos))
	{
		return true;
	}
	return false;
}


void Edge::serialize(OutputBlob& blob)
{
	blob.write(m_from->engine_cmp->uid);
	blob.write(m_to->engine_cmp->uid);
	blob.write(m_expression);
}


void Edge::deserialize(InputBlob& blob)
{
	int uid;
	blob.read(uid);
	m_from = (Node*)m_parent->getChildByUID(uid);
	blob.read(uid);
	m_to = (Node*)m_parent->getChildByUID(uid);
	blob.read(m_expression);
}


bool Edge::hitTest(const ImVec2& on_canvas_pos) const
{
	ImVec2 a = getEdgeStartPoint(m_from, m_to, true);
	ImVec2 b = getEdgeStartPoint(m_to, m_from, false);

	ImVec2 dif = a - b;
	float len_squared = dif.x * dif.x + dif.y * dif.y;
	float t = Math::clamp(dot(on_canvas_pos - a, b - a) / len_squared, 0.0f, 1.0f);
	const ImVec2 projection = a + (b - a) * t;
	ImVec2 dist_vec = on_canvas_pos - projection;

	return dot(dist_vec, dist_vec) < 100;
}


AnimationNode::AnimationNode(Anim::Component* engine_cmp, Container* parent, ControllerResource& controller)
	: Node(engine_cmp, parent, controller)
{
}


void AnimationNode::compile()
{
	auto* engine_node = (Anim::AnimationNode*)engine_cmp;
	Anim::InputDecl& decl = m_controller.getEngineResource()->getInputDecl();
	if (root_rotation_input >= 0)
	{
		engine_node->root_rotation_input_offset = decl.inputs[root_rotation_input].offset;
	}
	else
	{
		engine_node->root_rotation_input_offset = -1;
	}
}


void AnimationNode::debug(ImDrawList* draw, const ImVec2& canvas_screen_pos, Anim::ComponentInstance* runtime)
{
	if (runtime->source.type != engine_cmp->type) return;

	ImVec2 p = canvas_screen_pos + pos;
	p = p + ImVec2(5, ImGui::GetTextLineHeightWithSpacing() * 1.5f);
	draw->AddRect(p, p + ImVec2(size.x - 10, 5), 0xfff00fff);
	float t = Math::clamp(runtime->getTime() / runtime->getLength(), 0.0f, 1.0f);
	draw->AddRectFilled(p, p + ImVec2((size.x - 10) * t, 5), 0xfff00fff);
}


void AnimationNode::onGUI()
{
	Node::onGUI();
	
	auto* node = (Anim::AnimationNode*)engine_cmp;
	auto getter = [](void* data, int idx, const char** out) -> bool {
		auto* node = (AnimationNode*)data;
		auto& slots = node->m_controller.getAnimationSlots();
		*out = slots[idx].c_str();
		return true;
	};
	
	auto& slots = m_controller.getAnimationSlots();
	int current = 0;

	for (int i = 0; i < node->animations_hashes.size(); ++i)
	{
		for (current = 0; current < slots.size() && crc32(slots[current].c_str()) != node->animations_hashes[i]; ++current);
		ImGui::PushID(i);
		if (ImGui::Combo("Animation", &current, getter, this, slots.size()))
		{
			node->animations_hashes[i] = crc32(slots[current].c_str());
		}
		ImGui::SameLine();
		if (ImGui::Button("Remove"))
		{
			node->animations_hashes.erase(i);
		}
		ImGui::PopID();
	}
	if (ImGui::Button("Add animation"))
	{
		node->animations_hashes.emplace(0);
	}
	ImGui::Checkbox("Looped", &node->looped);

	Anim::InputDecl& decl = m_controller.getEngineResource()->getInputDecl();
	auto input_getter = [](void* data, int idx, const char** out) -> bool {
		auto& decl = *(Anim::InputDecl*)data;
		if (idx >= decl.inputs_count)
		{
			*out = "No root motion rotation";
		}
		else
		{
			*out = decl.inputs[idx].name;
		}
		return true;
	};
	if(ImGui::Combo("Root rotation input", &root_rotation_input, input_getter, &decl, decl.inputs_count + 1))
	{
		if (root_rotation_input >= decl.inputs_count) root_rotation_input = -1;
	}
}


struct EntryEdge : public Component
{
	EntryEdge(StateMachine* parent, Node* to, ControllerResource& controller)
		: Component(nullptr, parent, controller)
		, m_parent(parent)
		, m_to(to)
	{
		parent->getEntryNode()->entries.push(this);
		expression = "";
	}


	~EntryEdge()
	{
		m_parent->removeChild(this);
	}

	void serialize(Lumix::OutputBlob& blob) override {}
	void deserialize(Lumix::InputBlob& blob) override {}
	bool hitTest(const ImVec2& on_canvas_pos) const
	{
		ImVec2 a = getEdgeStartPoint(m_parent->getEntryNode(), m_to, true);
		ImVec2 b = getEdgeStartPoint(m_to, m_parent->getEntryNode(), false);

		ImVec2 dif = a - b;
		float len_squared = dif.x * dif.x + dif.y * dif.y;
		float t = Math::clamp(dot(on_canvas_pos - a, b - a) / len_squared, 0.0f, 1.0f);
		const ImVec2 projection = a + (b - a) * t;
		ImVec2 dist_vec = on_canvas_pos - projection;

		return dot(dist_vec, dist_vec) < 100;
	}


	void compile() override
	{
		// TODO
	}


	void onGUI() override
	{
		ImGui::InputText("Condition", expression.data, lengthOf(expression.data));
	}


	bool isNode() const override { return false; }


	bool draw(ImDrawList* draw, const ImVec2& canvas_screen_pos, bool selected) override
	{
		u32 color = ImGui::ColorConvertFloat4ToU32(
			selected ? ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered] : ImGui::GetStyle().Colors[ImGuiCol_Button]);
		ImVec2 from = getEdgeStartPoint(m_parent->getEntryNode(), m_to, true) + canvas_screen_pos;
		ImVec2 to = getEdgeStartPoint(m_to, m_parent->getEntryNode(), false) + canvas_screen_pos;
		draw->AddLine(from, to, color);
		ImVec2 dir = to - from;
		dir = dir * (1 / sqrt(dot(dir, dir))) * 5;
		ImVec2 right(dir.y, -dir.x);
		draw->AddLine(to, to - dir + right, color);
		draw->AddLine(to, to - dir - right, color);
		if (ImGui::IsMouseClicked(0) && hitTest(ImGui::GetMousePos() - canvas_screen_pos))
		{
			return true;
		}
		return false;
	}

	Node* getTo() const { return m_to; }

	StaticString<128> expression;

private:
	StateMachine* m_parent;
	Node* m_to;
};


EntryNode::EntryNode(Container* parent, ControllerResource& controller)
	: Node(nullptr, parent, controller)
	, entries(controller.getAllocator())
{
	m_name = "Entry";
}


StateMachine::StateMachine(Anim::Component* engine_cmp, Container* parent, ControllerResource& controller)
	: Container(engine_cmp, parent, controller)
{
	m_entry_node = LUMIX_NEW(controller.getAllocator(), EntryNode)(this, controller);
	m_editor_cmps.push(m_entry_node);
}


void StateMachine::removeChild(Component* component)
{
	Container::removeChild(component);
	auto* sm = (Anim::StateMachine*)engine_cmp;
	for(int i = 0; i < sm->entries.size(); ++i)
	{
		if (sm->entries[i].node == component->engine_cmp)
		{
			sm->entries.erase(i);
			LUMIX_DELETE(m_controller.getAllocator(), m_entry_node->entries[i]);
			break;
		}
	}
}


void StateMachine::onGUI()
{
	Container::onGUI();
	if (ImGui::Button("Show Children"))
	{
		m_controller.getEditor().setContainer(this);
	}
}


void StateMachine::compile()
{
	Container::compile();
	int i = 0;
	for (auto* entry : m_entry_node->entries)
	{
		auto* sm = (Anim::StateMachine*)engine_cmp;
		sm->entries[i].condition.compile(entry->expression, m_controller.getEngineResource()->getInputDecl());
		++i;
	}
}


static Component* createComponent(Anim::Component* engine_cmp, Container* parent, ControllerResource& controller)
{
	IAllocator& allocator = controller.getAllocator();
	switch (engine_cmp->type)
	{
		case Anim::Component::EDGE: return LUMIX_NEW(allocator, Edge)((Anim::Edge*)engine_cmp, parent, controller);
		case Anim::Component::SIMPLE_ANIMATION:
			return LUMIX_NEW(allocator, AnimationNode)(engine_cmp, parent, controller);
		case Anim::Component::STATE_MACHINE: return LUMIX_NEW(allocator, StateMachine)(engine_cmp, parent, controller);
		default: ASSERT(false); return nullptr;
	}
}


void Container::deserialize(InputBlob& blob)
{
	Node::deserialize(blob);
	int size;
	blob.read(size);
	for (int i = 0; i < size; ++i)
	{
		int uid;
		blob.read(uid);
		if (uid >= 0)
		{
			auto* engine_sm = (Anim::StateMachine*)engine_cmp;
			Component* cmp = createComponent(engine_sm->getChildByUID(uid), this, m_controller);
			cmp->deserialize(blob);
			m_editor_cmps.push(cmp);
		}
	}
}


void Container::compile()
{
	Node::compile();
	for (auto* cmp : m_editor_cmps)
	{
		cmp->compile();
	}
}


void Container::serialize(OutputBlob& blob)
{
	Node::serialize(blob);
	blob.write(m_editor_cmps.size());
	for (auto* cmp : m_editor_cmps)
	{
		blob.write(cmp->engine_cmp ? cmp->engine_cmp->uid : -1);
		if(cmp->engine_cmp) cmp->serialize(blob);
	}
}


void StateMachine::createState(Anim::Component::Type type, const ImVec2& pos)
{
	auto* cmp = (Node*)createComponent(Anim::createComponent(type, m_allocator), this, m_controller);
	cmp->pos = pos;
	cmp->size.x = 100;
	cmp->size.y = 30;
	cmp->engine_cmp->uid = m_controller.createUID();
	m_editor_cmps.push(cmp);
	((Anim::StateMachine*)engine_cmp)->children.push(cmp->engine_cmp);
	m_selected_component = cmp;
}


void StateMachine::deserialize(Lumix::InputBlob& blob)
{
	Container::deserialize(blob);
	m_entry_node->deserialize(blob);
	int count;
	blob.read(count);
	for (int i = 0; i < count; ++i)
	{
		int uid;
		blob.read(uid);
		Node* node = (Node*)getChildByUID(uid);
		auto* edge = LUMIX_NEW(m_allocator, EntryEdge)(this, node, m_controller);
		m_editor_cmps.push(edge);
		blob.read(edge->expression);
	}
}


void StateMachine::serialize(Lumix::OutputBlob& blob)
{
	Container::serialize(blob);
	m_entry_node->serialize(blob);
	blob.write(m_entry_node->entries.size());
	for (EntryEdge* edge : m_entry_node->entries)
	{
		blob.write(edge->getTo()->engine_cmp->uid);
		blob.write(edge->expression);
	}
}


void StateMachine::debugInside(ImDrawList* draw,
	const ImVec2& canvas_screen_pos,
	Anim::ComponentInstance* runtime,
	Container* current)
{
	if (runtime->source.type != Anim::Component::STATE_MACHINE) return;
	
	auto* child_runtime = ((Anim::StateMachineInstance*)runtime)->current;
	if (!child_runtime) return;
	auto* child = getChildByUID(child_runtime->source.uid);
	if (child)
	{
		if(current == this)
			child->debug(draw, canvas_screen_pos, child_runtime);
		else
			child->debugInside(draw, canvas_screen_pos, child_runtime, current);
	}
}


void StateMachine::debug(ImDrawList* draw, const ImVec2& canvas_screen_pos, Lumix::Anim::ComponentInstance* runtime)
{
	if (runtime->source.type != engine_cmp->type) return;

	ImVec2 p = canvas_screen_pos + pos;
	p = p + ImVec2(size.x * 0.5f - 3, ImGui::GetTextLineHeightWithSpacing() * 1.5f);
	draw->AddRectFilled(p, p + ImVec2(6, 6), 0xfff00FFF);
}


EntryEdge* StateMachine::createEntryEdge(Node* node)
{
	auto* edge = LUMIX_NEW(m_allocator, EntryEdge)(this, node, m_controller);
	m_editor_cmps.push(edge);

	auto* engine_sm = (Anim::StateMachine*)engine_cmp;
	auto& entry = engine_sm->entries.emplace(engine_sm->allocator);
	entry.node = (Anim::Node*)node->engine_cmp;
	return edge;
}


void StateMachine::drawInside(ImDrawList* draw, const ImVec2& canvas_screen_pos)
{
	if (ImGui::IsWindowHovered())
	{
		if (ImGui::IsMouseClicked(0)) m_selected_component = nullptr;
		if (ImGui::IsMouseReleased(1) && m_mouse_status == NONE)
		{
			m_context_cmp = nullptr;
			ImGui::OpenPopup("context_menu");
		}
	}

	for (int i = 0; i < m_editor_cmps.size(); ++i)
	{
		Component* cmp = m_editor_cmps[i];
		if (cmp->draw(draw, canvas_screen_pos, m_selected_component == cmp))
		{
			m_selected_component = cmp;
		}

		if (cmp->isNode() && ImGui::IsItemHovered())
		{
			if (ImGui::IsMouseClicked(0))
			{
				m_drag_source = (Node*)cmp;
				m_mouse_status = DOWN_LEFT;
			}
			if (ImGui::IsMouseClicked(1))
			{
				m_drag_source = (Node*)cmp;
				m_mouse_status = DOWN_RIGHT;
			}
		}

		if (m_mouse_status == DOWN_RIGHT && ImGui::IsMouseDragging(1)) m_mouse_status = NEW_EDGE;
		if (m_mouse_status == DOWN_LEFT && ImGui::IsMouseDragging(0)) m_mouse_status = DRAG_NODE;
	}

	if (ImGui::IsMouseReleased(1))
	{
		Component* hit_cmp = childrenHitTest(ImGui::GetMousePos() - canvas_screen_pos);
		if (hit_cmp)
		{
			if (m_mouse_status == NEW_EDGE)
			{
				if (hit_cmp != m_drag_source && hit_cmp->isNode())
				{
					if (hit_cmp == m_entry_node)
					{
						createEntryEdge(m_drag_source);
					}
					else if (m_drag_source == m_entry_node)
					{
						createEntryEdge((Node*)hit_cmp);
					}
					else
					{
						auto* engine_parent = ((Anim::Container*)engine_cmp);
						auto* engine_edge = LUMIX_NEW(m_allocator, Anim::Edge)(m_allocator);
						engine_edge->uid = m_controller.createUID();
						engine_edge->from = (Anim::Node*)m_drag_source->engine_cmp;
						engine_edge->to = (Anim::Node*)hit_cmp->engine_cmp;
						engine_parent->children.push(engine_edge);

						auto* edge = LUMIX_NEW(m_allocator, Edge)(engine_edge, this, m_controller);
						m_editor_cmps.push(edge);
						m_selected_component = edge;
					}
				}
			}
			else
			{
				m_context_cmp = hit_cmp;
				m_selected_component = hit_cmp;
				ImGui::OpenPopup("context_menu");
			}
		}
	}


	if (m_mouse_status == DRAG_NODE)
	{
		m_drag_source->pos = m_drag_source->pos + ImGui::GetIO().MouseDelta;
	}

	if (ImGui::IsMouseReleased(0) || ImGui::IsMouseReleased(1)) m_mouse_status = NONE;

	if (m_mouse_status == NEW_EDGE)
	{
		draw->AddLine(canvas_screen_pos + m_drag_source->pos + m_drag_source->size * 0.5f, ImGui::GetMousePos(), 0xfff00FFF);
	}

	if (ImGui::BeginPopup("context_menu"))
	{
		ImVec2 pos_on_canvas = ImGui::GetMousePos() - canvas_screen_pos;
		if (ImGui::BeginMenu("Create"))
		{
			if (ImGui::MenuItem("Simple")) createState(Anim::Component::SIMPLE_ANIMATION, pos_on_canvas);
			if (ImGui::MenuItem("State machine")) createState(Anim::Component::STATE_MACHINE, pos_on_canvas);
			ImGui::EndMenu();
		}
		if (m_context_cmp && m_context_cmp != m_entry_node)
		{
			if (ImGui::MenuItem("Remove"))
			{
				LUMIX_DELETE(m_controller.getAllocator(), m_context_cmp);
				if (m_selected_component == m_context_cmp) m_selected_component = nullptr;
				m_context_cmp = nullptr;
			}
		}
		ImGui::EndPopup();
	}
}


ControllerResource::ControllerResource(AnimationEditor& editor, ResourceManagerBase& manager, IAllocator& allocator)
	: m_animation_slots(allocator)
	, m_allocator(allocator)
	, m_editor(editor)
{
	m_engine_resource = LUMIX_NEW(allocator, Anim::ControllerResource)(Path("editor"), manager, allocator);
	auto* engine_root = LUMIX_NEW(allocator, Anim::StateMachine)(allocator);
	m_engine_resource->setRoot(engine_root);
	m_root = LUMIX_NEW(allocator, StateMachine)(engine_root, nullptr, *this);
}


ControllerResource::~ControllerResource()
{
	LUMIX_DELETE(m_allocator, m_engine_resource);
	LUMIX_DELETE(m_allocator, m_root);
}


void ControllerResource::serialize(OutputBlob& blob)
{
	m_root->compile();

	m_engine_resource->serialize(blob);

	blob.write(m_last_uid);
	m_root->serialize(blob);
	blob.write(m_animation_slots.size());
	for (auto& slot : m_animation_slots)
	{
		blob.writeString(slot.c_str());
	}
}


bool ControllerResource::deserialize(InputBlob& blob, Engine& engine, IAllocator& allocator)
{
	LUMIX_DELETE(m_allocator, m_engine_resource);
	LUMIX_DELETE(m_allocator, m_root);
	auto* manager = engine.getResourceManager().get(CONTROLLER_RESOURCE_TYPE);
	m_engine_resource = LUMIX_NEW(allocator, Anim::ControllerResource)(Path("editor"), *manager, allocator);
	m_engine_resource->create();
	if (!m_engine_resource->deserialize(blob)) return false;

	blob.read(m_last_uid);
	m_root = createComponent(m_engine_resource->getRoot(), nullptr, *this);
	m_root->deserialize(blob);

	int count;
	blob.read(count);
	m_animation_slots.clear();
	for (int i = 0; i < count; ++i)
	{
		auto& slot = m_animation_slots.emplace(allocator);
		char tmp[64];
		blob.readString(tmp, lengthOf(tmp));
		slot = tmp;
	}
	return true;
}


} // namespace AnimEditor