#include "anim/AnimatorSerializer.h"

#include "core/Logger.h"

#include <json.hpp>

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace
{
    using json = nlohmann::json;

    const char* ToStr(engine::anim::ParameterType t)
    {
        using engine::anim::ParameterType;
        switch (t)
        {
            case ParameterType::Bool:    return "Bool";
            case ParameterType::Int:     return "Int";
            case ParameterType::Float:   return "Float";
            case ParameterType::Trigger: return "Trigger";
        }
        return "Bool";
    }

    engine::anim::ParameterType ParseParamType(const std::string& s)
    {
        using engine::anim::ParameterType;
        if (s == "Bool")    { return ParameterType::Bool; }
        if (s == "Int")     { return ParameterType::Int; }
        if (s == "Float")   { return ParameterType::Float; }
        if (s == "Trigger") { return ParameterType::Trigger; }
        engine::core::LogInfoA("[anim] WARN: unknown ParameterType '");
        engine::core::LogInfoA(s.c_str());
        engine::core::LogInfoA("' -> fallback Bool\n");
        return ParameterType::Bool;
    }

    const char* ToStr(engine::anim::ConditionOp op)
    {
        using engine::anim::ConditionOp;
        switch (op)
        {
            case ConditionOp::IfTrue:    return "IfTrue";
            case ConditionOp::IfFalse:   return "IfFalse";
            case ConditionOp::Greater:   return "Greater";
            case ConditionOp::Less:      return "Less";
            case ConditionOp::Equals:    return "Equals";
            case ConditionOp::NotEquals: return "NotEquals";
        }
        return "IfTrue";
    }

    engine::anim::ConditionOp ParseConditionOp(const std::string& s)
    {
        using engine::anim::ConditionOp;
        if (s == "IfTrue")    { return ConditionOp::IfTrue; }
        if (s == "IfFalse")   { return ConditionOp::IfFalse; }
        if (s == "Greater")   { return ConditionOp::Greater; }
        if (s == "Less")      { return ConditionOp::Less; }
        if (s == "Equals")    { return ConditionOp::Equals; }
        if (s == "NotEquals") { return ConditionOp::NotEquals; }
        engine::core::LogInfoA("[anim] WARN: unknown ConditionOp '");
        engine::core::LogInfoA(s.c_str());
        engine::core::LogInfoA("' -> fallback IfTrue\n");
        return ConditionOp::IfTrue;
    }
}

namespace engine::anim
{
    void SaveJson(const AnimatorController& c, std::string_view path)
    {
        json root;
        root["name"]             = c.name;
        root["defaultStateName"] = c.defaultStateName;

        json states = json::array();
        for (const auto& s : c.states)
        {
            json e;
            e["name"]           = s.name;
            e["motionClipPath"] = s.motionClipPath;
            e["loop"]           = s.loop;
            e["speed"]          = s.speed;
            // Blend tree — entries 가 있을 때만 직렬화 (기존 단일 clip state JSON 호환).
            if (!s.blendTree.empty())
            {
                json bt = json::array();
                for (const auto& entry : s.blendTree)
                {
                    json en;
                    en["motionClipPath"] = entry.motionClipPath;
                    en["threshold"]      = entry.threshold;
                    bt.push_back(std::move(en));
                }
                e["blendTree"]      = std::move(bt);
                e["blendParameter"] = s.blendParameter;
            }
            // Root motion — 명시된 경우만 직렬화 (기존 JSON 과 호환).
            if (s.hasRootMotion)
            {
                json rm;
                rm["takeoffNormTime"] = s.rootMotion.takeoffNormTime;
                rm["landingNormTime"] = s.rootMotion.landingNormTime;
                rm["peakHeight"]      = s.rootMotion.peakHeight;
                rm["fadeWindow"]      = s.rootMotion.fadeWindow;
                e["rootMotion"]       = std::move(rm);
            }
            states.push_back(std::move(e));
        }
        root["states"] = std::move(states);

        json params = json::array();
        for (const auto& p : c.parameters)
        {
            json e;
            e["name"]         = p.name;
            e["type"]         = ToStr(p.type);
            e["defaultValue"] = p.defaultValue;
            params.push_back(std::move(e));
        }
        root["parameters"] = std::move(params);

        json transitions = json::array();
        for (const auto& t : c.transitions)
        {
            json e;
            e["fromStateName"]     = t.fromStateName;   // 빈 문자열 = Any State
            e["toStateName"]       = t.toStateName;
            e["crossfadeDuration"] = t.crossfadeDuration;
            e["hasExitTime"]       = t.hasExitTime;
            e["exitTime"]          = t.exitTime;

            json conds = json::array();
            for (const auto& cd : t.conditions)
            {
                json ce;
                ce["parameterName"] = cd.parameterName;
                ce["op"]            = ToStr(cd.op);
                ce["value"]         = cd.value;
                conds.push_back(std::move(ce));
            }
            e["conditions"] = std::move(conds);

            transitions.push_back(std::move(e));
        }
        root["transitions"] = std::move(transitions);

        std::ofstream ofs{ std::string{ path }, std::ios::binary };
        if (!ofs)
        {
            std::ostringstream oss;
            oss << "AnimatorSerializer::SaveJson: open failed (" << path << ")";
            throw std::runtime_error(oss.str());
        }
        ofs << root.dump(2);
    }

    AnimatorController LoadJson(std::string_view path)
    {
        std::ifstream ifs{ std::string{ path }, std::ios::binary };
        if (!ifs)
        {
            std::ostringstream oss;
            oss << "AnimatorSerializer::LoadJson: open failed (" << path << ")";
            throw std::runtime_error(oss.str());
        }

        json root;
        try
        {
            ifs >> root;
        }
        catch (const json::parse_error& e)
        {
            std::ostringstream oss;
            oss << "AnimatorSerializer::LoadJson: parse failed (" << path << "): " << e.what();
            throw std::runtime_error(oss.str());
        }

        AnimatorController c;
        if (auto it = root.find("name");             it != root.end() && it->is_string()) { c.name = it->get<std::string>(); }
        if (auto it = root.find("defaultStateName"); it != root.end() && it->is_string()) { c.defaultStateName = it->get<std::string>(); }

        if (auto it = root.find("states"); it != root.end() && it->is_array())
        {
            for (const auto& e : *it)
            {
                AnimatorState s;
                if (auto x = e.find("name");           x != e.end() && x->is_string())  { s.name = x->get<std::string>(); }
                if (auto x = e.find("motionClipPath"); x != e.end() && x->is_string())  { s.motionClipPath = x->get<std::string>(); }
                if (auto x = e.find("loop");           x != e.end() && x->is_boolean()) { s.loop = x->get<bool>(); }
                if (auto x = e.find("speed");          x != e.end() && x->is_number())  { s.speed = x->get<float>(); }
                if (auto x = e.find("blendParameter"); x != e.end() && x->is_string())  { s.blendParameter = x->get<std::string>(); }
                if (auto x = e.find("blendTree");      x != e.end() && x->is_array())
                {
                    for (const auto& en : *x)
                    {
                        BlendTreeEntry entry;
                        if (auto y = en.find("motionClipPath"); y != en.end() && y->is_string()) { entry.motionClipPath = y->get<std::string>(); }
                        if (auto y = en.find("threshold");      y != en.end() && y->is_number()) { entry.threshold      = y->get<float>(); }
                        s.blendTree.push_back(std::move(entry));
                    }
                }
                if (auto x = e.find("rootMotion"); x != e.end() && x->is_object())
                {
                    s.hasRootMotion = true;
                    if (auto y = x->find("takeoffNormTime"); y != x->end() && y->is_number()) { s.rootMotion.takeoffNormTime = y->get<float>(); }
                    if (auto y = x->find("landingNormTime"); y != x->end() && y->is_number()) { s.rootMotion.landingNormTime = y->get<float>(); }
                    if (auto y = x->find("peakHeight");      y != x->end() && y->is_number()) { s.rootMotion.peakHeight      = y->get<float>(); }
                    if (auto y = x->find("fadeWindow");      y != x->end() && y->is_number()) { s.rootMotion.fadeWindow      = y->get<float>(); }
                }
                c.states.push_back(std::move(s));
            }
        }

        if (auto it = root.find("parameters"); it != root.end() && it->is_array())
        {
            for (const auto& e : *it)
            {
                AnimatorParameter p;
                if (auto x = e.find("name");         x != e.end() && x->is_string()) { p.name = x->get<std::string>(); }
                if (auto x = e.find("type");         x != e.end() && x->is_string()) { p.type = ParseParamType(x->get<std::string>()); }
                if (auto x = e.find("defaultValue"); x != e.end() && x->is_number()) { p.defaultValue = x->get<float>(); }
                c.parameters.push_back(std::move(p));
            }
        }

        if (auto it = root.find("transitions"); it != root.end() && it->is_array())
        {
            for (const auto& e : *it)
            {
                AnimatorTransition t;
                if (auto x = e.find("fromStateName");     x != e.end() && x->is_string())  { t.fromStateName = x->get<std::string>(); }
                if (auto x = e.find("toStateName");       x != e.end() && x->is_string())  { t.toStateName = x->get<std::string>(); }
                if (auto x = e.find("crossfadeDuration"); x != e.end() && x->is_number())  { t.crossfadeDuration = x->get<float>(); }
                if (auto x = e.find("hasExitTime");       x != e.end() && x->is_boolean()) { t.hasExitTime = x->get<bool>(); }
                if (auto x = e.find("exitTime");          x != e.end() && x->is_number())  { t.exitTime = x->get<float>(); }

                if (auto cit = e.find("conditions"); cit != e.end() && cit->is_array())
                {
                    for (const auto& ce : *cit)
                    {
                        TransitionCondition cd;
                        if (auto x = ce.find("parameterName"); x != ce.end() && x->is_string()) { cd.parameterName = x->get<std::string>(); }
                        if (auto x = ce.find("op");            x != ce.end() && x->is_string()) { cd.op = ParseConditionOp(x->get<std::string>()); }
                        if (auto x = ce.find("value");         x != ce.end() && x->is_number()) { cd.value = x->get<float>(); }
                        t.conditions.push_back(std::move(cd));
                    }
                }
                c.transitions.push_back(std::move(t));
            }
        }

        return c;
    }
}
