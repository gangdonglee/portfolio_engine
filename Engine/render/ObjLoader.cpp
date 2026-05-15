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
        // "v//vn" / "v/vt/vn" / "v" 형식의 face vertex 토큰 파싱.
        struct FaceVertex { int32 posIdx = -1; int32 normIdx = -1; };

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
                return fv;
            }
            // "v//vn" 또는 "v/vt/vn"
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

        std::vector<DirectX::XMFLOAT3> positions;
        std::vector<DirectX::XMFLOAT3> normals;
        std::vector<FaceVertex>        faceVertices;  // 모든 face 정점 (순서대로 삼각형 인덱스)

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
            else if (tag == "f")
            {
                // 삼각형 가정 — 3개 토큰.
                std::string a, b, c;
                iss >> a >> b >> c;
                if (a.empty() || b.empty() || c.empty())
                {
                    throw std::runtime_error("OBJ: f 라인이 삼각형이 아님 (n-gon 미지원)");
                }
                faceVertices.push_back(ParseFaceVertex(a));
                faceVertices.push_back(ParseFaceVertex(b));
                faceVertices.push_back(ParseFaceVertex(c));
            }
            // vt 등 기타 라인 무시.
        }

        if (positions.empty() || faceVertices.empty())
        {
            throw std::runtime_error("OBJ: 유효한 정점/면 없음");
        }

        // (posIdx, normIdx) 쌍을 키로 unique vertex 생성.
        std::map<std::pair<int32, int32>, uint16> dedup;
        std::vector<Mesh::Vertex> vertices;
        std::vector<uint16>       indices;

        for (const FaceVertex& fv : faceVertices)
        {
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

            const std::pair<int32, int32> key{ fv.posIdx, fv.normIdx };
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
            v.color = defaultColor;

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
                      L"[render] OBJ loaded: positions=%zu, normals=%zu, vertices=%zu, indices=%zu\n",
                      positions.size(), normals.size(), vertices.size(), indices.size());
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
