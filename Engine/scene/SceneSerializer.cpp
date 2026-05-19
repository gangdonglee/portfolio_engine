#include "scene/SceneSerializer.h"

#include <json.hpp>

#include <DirectXMath.h>

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace
{
    using json = nlohmann::json;

    // float3/float4 ↔ json array 변환. ADL 로 to_json/from_json 패턴을 쓰면 DirectX 의
    // POD 타입이 nlohmann 의 네임스페이스 분리(ADL 못 찾음) 문제로 깔끔하지 않아
    // 본 파일 안에서만 헬퍼로 처리.
    json ToArray(const DirectX::XMFLOAT3& v) { return json::array({ v.x, v.y, v.z }); }
    json ToArray(const DirectX::XMFLOAT4& v) { return json::array({ v.x, v.y, v.z, v.w }); }

    DirectX::XMFLOAT3 ParseFloat3(const json& j, DirectX::XMFLOAT3 fallback = {})
    {
        if (!j.is_array() || j.size() < 3) { return fallback; }
        return { j[0].get<float>(), j[1].get<float>(), j[2].get<float>() };
    }
    DirectX::XMFLOAT4 ParseFloat4(const json& j, DirectX::XMFLOAT4 fallback = { 0,0,0,1 })
    {
        if (!j.is_array() || j.size() < 4) { return fallback; }
        return { j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>() };
    }

    // 헬퍼: 키가 없으면 fallback. 있으면 변환.
    template <typename T, typename Fn>
    T Get(const json& j, const char* key, T fallback, Fn convert)
    {
        if (auto it = j.find(key); it != j.end()) { return convert(*it); }
        return fallback;
    }
}

namespace engine::scene
{
    void SaveJson(const Scene& scene, std::string_view path)
    {
        json root;
        root["name"]    = scene.name;
        root["ambient"] = ToArray(scene.ambient);

        json camStart;
        camStart["position"] = ToArray(scene.cameraStart.position);
        camStart["target"]   = ToArray(scene.cameraStart.target);
        camStart["fovYRad"]  = scene.cameraStart.fovYRad;
        root["cameraStart"]  = std::move(camStart);

        json meshes = json::array();
        for (const auto& m : scene.meshes)
        {
            json t;
            t["position"] = ToArray(m.transform.position);
            t["rotation"] = ToArray(m.transform.rotation);
            t["scale"]    = ToArray(m.transform.scale);

            json e;
            e["name"]          = m.name;
            e["meshAssetPath"] = m.meshAssetPath;
            if (!m.animationClipPath.empty())
            {
                e["animationClipPath"] = m.animationClipPath;
            }
            e["transform"]     = std::move(t);
            meshes.push_back(std::move(e));
        }
        root["meshes"] = std::move(meshes);

        json dirLights = json::array();
        for (const auto& d : scene.dirLights)
        {
            json e;
            e["name"]        = d.name;
            e["directionWS"] = ToArray(d.directionWS);
            e["color"]       = ToArray(d.color);
            e["intensity"]   = d.intensity;
            dirLights.push_back(std::move(e));
        }
        root["dirLights"] = std::move(dirLights);

        json pointLights = json::array();
        for (const auto& p : scene.pointLights)
        {
            json e;
            e["name"]       = p.name;
            e["positionWS"] = ToArray(p.positionWS);
            e["color"]      = ToArray(p.color);
            e["intensity"]  = p.intensity;
            e["range"]      = p.range;
            pointLights.push_back(std::move(e));
        }
        root["pointLights"] = std::move(pointLights);

        std::ofstream ofs{ std::string{ path }, std::ios::binary };
        if (!ofs)
        {
            std::ostringstream oss;
            oss << "SaveJson: open failed (" << path << ")";
            throw std::runtime_error(oss.str());
        }
        // dump(2) — 2-space indent. git diff 가독.
        ofs << root.dump(2);
    }

    Scene LoadJson(std::string_view path)
    {
        std::ifstream ifs{ std::string{ path }, std::ios::binary };
        if (!ifs)
        {
            std::ostringstream oss;
            oss << "LoadJson: open failed (" << path << ")";
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
            oss << "LoadJson: parse failed (" << path << "): " << e.what();
            throw std::runtime_error(oss.str());
        }

        Scene scene;
        if (auto it = root.find("name");    it != root.end() && it->is_string()) { scene.name = it->get<std::string>(); }
        if (auto it = root.find("ambient"); it != root.end()) { scene.ambient = ParseFloat3(*it, scene.ambient); }

        if (auto it = root.find("cameraStart"); it != root.end() && it->is_object())
        {
            const auto& cs = *it;
            scene.cameraStart.position = Get<DirectX::XMFLOAT3>(cs, "position", scene.cameraStart.position,
                [](const json& j){ return ParseFloat3(j); });
            scene.cameraStart.target   = Get<DirectX::XMFLOAT3>(cs, "target", scene.cameraStart.target,
                [](const json& j){ return ParseFloat3(j); });
            scene.cameraStart.fovYRad  = Get<float>(cs, "fovYRad", scene.cameraStart.fovYRad,
                [](const json& j){ return j.get<float>(); });
        }

        if (auto it = root.find("meshes"); it != root.end() && it->is_array())
        {
            for (const auto& e : *it)
            {
                MeshInstance m;
                if (auto nit = e.find("name");              nit != e.end() && nit->is_string()) { m.name = nit->get<std::string>(); }
                if (auto pit = e.find("meshAssetPath");     pit != e.end() && pit->is_string()) { m.meshAssetPath = pit->get<std::string>(); }
                if (auto cit = e.find("animationClipPath"); cit != e.end() && cit->is_string()) { m.animationClipPath = cit->get<std::string>(); }
                if (auto tit = e.find("transform"); tit != e.end() && tit->is_object())
                {
                    m.transform.position = Get<DirectX::XMFLOAT3>(*tit, "position", m.transform.position,
                        [](const json& j){ return ParseFloat3(j); });
                    m.transform.rotation = Get<DirectX::XMFLOAT4>(*tit, "rotation", m.transform.rotation,
                        [](const json& j){ return ParseFloat4(j); });
                    m.transform.scale    = Get<DirectX::XMFLOAT3>(*tit, "scale", m.transform.scale,
                        [](const json& j){ return ParseFloat3(j); });
                }
                scene.meshes.push_back(std::move(m));
            }
        }

        if (auto it = root.find("dirLights"); it != root.end() && it->is_array())
        {
            for (const auto& e : *it)
            {
                DirectionalLight d;
                if (auto nit = e.find("name"); nit != e.end() && nit->is_string()) { d.name = nit->get<std::string>(); }
                d.directionWS = Get<DirectX::XMFLOAT3>(e, "directionWS", d.directionWS,
                    [](const json& j){ return ParseFloat3(j); });
                d.color       = Get<DirectX::XMFLOAT3>(e, "color", d.color,
                    [](const json& j){ return ParseFloat3(j); });
                d.intensity   = Get<float>(e, "intensity", d.intensity,
                    [](const json& j){ return j.get<float>(); });
                scene.dirLights.push_back(std::move(d));
            }
        }

        if (auto it = root.find("pointLights"); it != root.end() && it->is_array())
        {
            for (const auto& e : *it)
            {
                PointLight p;
                if (auto nit = e.find("name"); nit != e.end() && nit->is_string()) { p.name = nit->get<std::string>(); }
                p.positionWS = Get<DirectX::XMFLOAT3>(e, "positionWS", p.positionWS,
                    [](const json& j){ return ParseFloat3(j); });
                p.color      = Get<DirectX::XMFLOAT3>(e, "color", p.color,
                    [](const json& j){ return ParseFloat3(j); });
                p.intensity  = Get<float>(e, "intensity", p.intensity,
                    [](const json& j){ return j.get<float>(); });
                p.range      = Get<float>(e, "range", p.range,
                    [](const json& j){ return j.get<float>(); });
                scene.pointLights.push_back(std::move(p));
            }
        }

        return scene;
    }
}
