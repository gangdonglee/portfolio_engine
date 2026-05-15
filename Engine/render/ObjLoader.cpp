#include "render/ObjLoader.h"

#include "core/Logger.h"
#include "core/Types.h"
#include "render/Mesh.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace engine::render::obj_loader
{
    namespace
    {
        // "v" / "v/vt" / "v//vn" / "v/vt/vn" 형식의 face vertex 토큰 파싱.
        struct FaceVertex { int32 posIdx = -1; int32 uvIdx = -1; int32 normIdx = -1; };

        // .mtl 파일에서 newmtl/Kd 쌍을 파싱. 그 외 라인(Ka/Ks/Ns/map_Kd 등)은 무시.
        // 파일 부재/포맷 오류는 fatal 이 아님 — 빈 테이블 반환 → 호출자가 defaultColor 로 폴백.
        std::map<std::string, DirectX::XMFLOAT3> LoadMtl(const std::wstring& mtlPath)
        {
            std::map<std::string, DirectX::XMFLOAT3> materials;
            std::ifstream file(mtlPath);
            if (!file.is_open())
            {
                wchar_t buf[MAX_PATH + 64];
                std::swprintf(buf, std::size(buf),
                              L"[render] MTL 파일 열기 실패 (조용히 폴백): %ls\n",
                              mtlPath.c_str());
                engine::core::LogInfo(buf);
                return materials;
            }

            std::string             curName;
            DirectX::XMFLOAT3       curKd{ 1.0f, 1.0f, 1.0f };
            bool                    hasCurrent = false;

            auto flush = [&]() {
                if (hasCurrent && !curName.empty())
                {
                    materials[curName] = curKd;
                }
            };

            std::string line;
            while (std::getline(file, line))
            {
                if (line.empty() || line[0] == '#') continue;
                std::istringstream iss(line);
                std::string tag;
                iss >> tag;

                if (tag == "newmtl")
                {
                    flush();
                    iss >> curName;
                    curKd = { 1.0f, 1.0f, 1.0f };
                    hasCurrent = true;
                }
                else if (tag == "Kd")
                {
                    iss >> curKd.x >> curKd.y >> curKd.z;
                }
                // Ka/Ks/Ns/map_Kd/illum 등 모두 무시
            }
            flush();
            return materials;
        }

        FaceVertex ParseFaceVertex(const std::string& token)
        {
            FaceVertex fv;
            const size_t firstSlash = token.find('/');
            if (firstSlash == std::string::npos)
            {
                // "v" 만
                fv.posIdx = std::stoi(token);
                return fv;
            }
            fv.posIdx = std::stoi(token.substr(0, firstSlash));

            const size_t lastSlash = token.rfind('/');
            if (lastSlash == firstSlash)
            {
                // "v/vt" — normal 없음
                const std::string uvStr = token.substr(firstSlash + 1);
                if (!uvStr.empty())
                {
                    fv.uvIdx = std::stoi(uvStr);
                }
                return fv;
            }
            // "v//vn" 또는 "v/vt/vn"
            // 가운데 vt 부분 — firstSlash+1 .. lastSlash (비어있을 수 있음)
            if (lastSlash > firstSlash + 1)
            {
                const std::string uvStr = token.substr(firstSlash + 1, lastSlash - firstSlash - 1);
                if (!uvStr.empty())
                {
                    fv.uvIdx = std::stoi(uvStr);
                }
            }
            const std::string normStr = token.substr(lastSlash + 1);
            if (!normStr.empty())
            {
                fv.normIdx = std::stoi(normStr);
            }
            return fv;
        }
    }

    std::unique_ptr<Mesh> LoadObj(
        Device&                  device,
        const wchar_t*           absolutePath,
        const DirectX::XMFLOAT3& defaultColor)
    {
        std::ifstream file(absolutePath);
        if (!file.is_open())
        {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "OBJ 파일 열기 실패 (경로 손상 또는 파일 부재)");
            throw std::runtime_error(buf);
        }

        // OBJ 가 위치한 디렉터리 — mtllib 의 .mtl 파일 경로 계산용.
        std::wstring objDir;
        {
            std::wstring path(absolutePath);
            const size_t sep = path.find_last_of(L"\\/");
            objDir = (sep == std::wstring::npos) ? std::wstring{} : path.substr(0, sep + 1);
        }

        std::vector<DirectX::XMFLOAT3>     positions;
        std::vector<DirectX::XMFLOAT3>     normals;
        std::vector<DirectX::XMFLOAT2>     uvs;
        std::vector<FaceVertex>            faceVertices;        // 모든 face 정점 (순서대로 삼각형 인덱스)
        std::vector<DirectX::XMFLOAT3>     faceVertexColors;    // faceVertices 와 같은 길이 — 현재 머티리얼 색

        std::map<std::string, DirectX::XMFLOAT3> materials;
        DirectX::XMFLOAT3                  currentColor = defaultColor;

        std::string line;
        while (std::getline(file, line))
        {
            if (line.empty() || line[0] == '#') continue;

            std::istringstream iss(line);
            std::string tag;
            iss >> tag;

            if (tag == "v")
            {
                float x{}, y{}, z{};
                iss >> x >> y >> z;
                positions.push_back({ x, y, z });
            }
            else if (tag == "vn")
            {
                float x{}, y{}, z{};
                iss >> x >> y >> z;
                normals.push_back({ x, y, z });
            }
            else if (tag == "vt")
            {
                // OBJ 의 vt 는 u v [w] — w 는 무시.
                // OBJ 의 V축은 bottom-up. D3D 텍스처는 top-down 이므로 (1 - v) 로 뒤집어 저장.
                float u{}, v{};
                iss >> u >> v;
                uvs.push_back({ u, 1.0f - v });
            }
            else if (tag == "mtllib")
            {
                // 같은 폴더의 .mtl 파일 로드. ASCII 파일명 가정.
                std::string mtlName;
                iss >> mtlName;
                if (!mtlName.empty())
                {
                    const std::wstring mtlPath = objDir + std::wstring(mtlName.begin(), mtlName.end());
                    auto loaded = LoadMtl(mtlPath);
                    for (auto& kv : loaded) { materials[kv.first] = kv.second; }
                }
            }
            else if (tag == "usemtl")
            {
                std::string name;
                iss >> name;
                auto it = materials.find(name);
                currentColor = (it != materials.end()) ? it->second : defaultColor;
            }
            else if (tag == "f")
            {
                // n-gon fan triangulation — 정점 N (≥3) 개를 (v0, v_i, v_{i+1}) 삼각형 (N-2) 개로 분해.
                // 볼록 다각형 가정. 오목 polygon 은 self-intersection 가능 — 향후 ear clipping 으로 확장.
                std::vector<std::string> tokens;
                tokens.reserve(4);
                std::string tok;
                while (iss >> tok) { tokens.push_back(tok); }
                if (tokens.size() < 3)
                {
                    throw std::runtime_error("OBJ: f 라인 정점 수 < 3");
                }

                const FaceVertex v0 = ParseFaceVertex(tokens[0]);
                for (size_t i = 1; i + 1 < tokens.size(); ++i)
                {
                    const FaceVertex vi  = ParseFaceVertex(tokens[i]);
                    const FaceVertex vii = ParseFaceVertex(tokens[i + 1]);
                    faceVertices.push_back(v0);
                    faceVertices.push_back(vi);
                    faceVertices.push_back(vii);
                    faceVertexColors.push_back(currentColor);
                    faceVertexColors.push_back(currentColor);
                    faceVertexColors.push_back(currentColor);
                }
            }
            // 기타 라인 (o/s/g 등) 무시.
        }

        if (positions.empty() || faceVertices.empty())
        {
            throw std::runtime_error("OBJ: 유효한 정점/면 없음");
        }

        // (posIdx, uvIdx, normIdx) 트리플을 키로 unique vertex 생성.
        struct Key { int32 pos; int32 uv; int32 norm; };
        struct KeyLess {
            bool operator()(const Key& a, const Key& b) const noexcept {
                if (a.pos != b.pos)   return a.pos  < b.pos;
                if (a.uv  != b.uv)    return a.uv   < b.uv;
                return a.norm < b.norm;
            }
        };
        std::map<Key, uint16, KeyLess> dedup;
        std::vector<Mesh::Vertex> vertices;
        std::vector<uint16>       indices;

        for (size_t fvIdx = 0; fvIdx < faceVertices.size(); ++fvIdx)
        {
            const FaceVertex&        fv    = faceVertices[fvIdx];
            const DirectX::XMFLOAT3& color = faceVertexColors[fvIdx];

            // OBJ 는 1-based 인덱스. 음수 인덱스는 끝에서부터(역방향) — 현 구현 미지원.
            if (fv.posIdx <= 0)
            {
                throw std::runtime_error("OBJ: 음수/제로 위치 인덱스는 미지원");
            }
            const int32 posIdx = fv.posIdx - 1;
            if (posIdx >= static_cast<int32>(positions.size()))
            {
                throw std::runtime_error("OBJ: position 인덱스 범위 초과");
            }

            const Key key{ fv.posIdx, fv.uvIdx, fv.normIdx };
            auto it = dedup.find(key);
            if (it != dedup.end())
            {
                indices.push_back(it->second);
                continue;
            }

            Mesh::Vertex v{};
            v.position = positions[posIdx];
            if (fv.normIdx > 0 && static_cast<size_t>(fv.normIdx - 1) < normals.size())
            {
                v.normal = normals[fv.normIdx - 1];
            }
            else
            {
                v.normal = { 0.0f, 1.0f, 0.0f };  // normal 없으면 디폴트 +Y
            }
            if (fv.uvIdx > 0 && static_cast<size_t>(fv.uvIdx - 1) < uvs.size())
            {
                v.uv = uvs[fv.uvIdx - 1];
            }
            else
            {
                v.uv = { 0.0f, 0.0f };  // uv 없으면 디폴트 (0,0)
            }
            // 머티리얼이 적용된 면이면 그 색, 아니면 defaultColor (faceVertexColors 가 currentColor 로 채워짐).
            v.color = color;

            if (vertices.size() >= 65535)
            {
                throw std::runtime_error("OBJ: 정점 수가 65535 초과 — R16 인덱스 한계 (R32 마이그레이션 필요)");
            }
            const uint16 newIdx = static_cast<uint16>(vertices.size());
            vertices.push_back(v);
            dedup.emplace(key, newIdx);
            indices.push_back(newIdx);
        }

        wchar_t logLine[256];
        std::swprintf(logLine, std::size(logLine),
                      L"[render] OBJ loaded: positions=%zu, normals=%zu, uvs=%zu, materials=%zu, vertices=%zu, indices=%zu\n",
                      positions.size(), normals.size(), uvs.size(),
                      materials.size(), vertices.size(), indices.size());
        engine::core::LogInfo(logLine);

        return std::make_unique<Mesh>(
            device,
            vertices.data(),
            static_cast<uint32>(vertices.size()),
            indices.data(),
            static_cast<uint32>(indices.size()));
    }

    std::wstring DefaultAssetsDir()
    {
        wchar_t buf[MAX_PATH];
        const DWORD len = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
        if (len == 0 || len == MAX_PATH)
        {
            throw std::runtime_error("GetModuleFileNameW failed");
        }
        std::wstring path(buf, len);
        const auto sep = path.find_last_of(L"\\/");
        if (sep == std::wstring::npos)
        {
            throw std::runtime_error("Executable path has no separator");
        }
        return path.substr(0, sep + 1) + L"assets\\";
    }
}
