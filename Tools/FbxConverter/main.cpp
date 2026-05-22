// FbxConverter — Scene JSON 의 importTransform 을 FBX 자산에 baked-in 한다.
//
// 사용:
//   FbxConverter.exe -i <input.fbx> -o <output.fbx>
//                    --scene <scene.json> --mesh-name <MeshInstance.name>
//
// 동작:
//   1) Scene JSON 로드 → meshes[] 에서 name 매칭 → importTransform 추출
//   2) FBX 로드 (FbxImporter)
//   3) FbxScene 의 root 자식 노드의 LclTransform 에 importMatrix prepend
//      → mesh control point, bone bind pose (cluster TransformMatrix/TransformLinkMatrix),
//        animation keyframe transform 모두 FBX SDK 가 일관 변환된 상태로 export.
//   4) FbxExporter 로 새 FBX 출력 (binary)
//
// 결과:
//   *변환된 FBX 자산* 은 importTransform 없이 *원래 좌표계로 정상 표시*.
//   엔진 측 SceneRuntime 의 importTransform 합성은 identity 로 두면 됨.

#include "scene/SceneSerializer.h"
#include "scene/Scene.h"

#include <fbxsdk.h>

#include <DirectXMath.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace
{
    struct Args
    {
        std::string input;
        std::string output;
        std::string scenePath;
        std::string meshName;
    };

    void PrintUsage()
    {
        std::cout
            << "FbxConverter — Scene JSON 의 importTransform 을 FBX 자산에 baked-in.\n"
            << "\n"
            << "사용:\n"
            << "  FbxConverter.exe -i <input.fbx> -o <output.fbx>\n"
            << "                   --scene <scene.json> --mesh-name <MeshInstance.name>\n";
    }

    bool ParseArgs(int argc, char** argv, Args& out)
    {
        for (int i = 1; i < argc; ++i)
        {
            std::string_view a = argv[i];
            auto next = [&](const char* what) -> const char*
            {
                if (i + 1 >= argc)
                {
                    std::cerr << "[FbxConverter] missing value for " << what << "\n";
                    return nullptr;
                }
                return argv[++i];
            };
            if (a == "-i") { auto v = next("-i"); if (!v) return false; out.input = v; }
            else if (a == "-o") { auto v = next("-o"); if (!v) return false; out.output = v; }
            else if (a == "--scene") { auto v = next("--scene"); if (!v) return false; out.scenePath = v; }
            else if (a == "--mesh-name") { auto v = next("--mesh-name"); if (!v) return false; out.meshName = v; }
            else if (a == "-h" || a == "--help") { PrintUsage(); return false; }
            else
            {
                std::cerr << "[FbxConverter] unknown arg: " << a << "\n";
                return false;
            }
        }
        if (out.input.empty() || out.output.empty() || out.scenePath.empty() || out.meshName.empty())
        {
            std::cerr << "[FbxConverter] required: -i, -o, --scene, --mesh-name\n";
            return false;
        }
        return true;
    }

    // Scene JSON 에서 meshName 매칭 인스턴스의 importTransform 추출.
    bool FindImportTransform(const std::string& scenePath,
                             const std::string& meshName,
                             engine::scene::Transform& outImport)
    {
        engine::scene::Scene scene = engine::scene::LoadJson(scenePath);
        for (const auto& m : scene.meshes)
        {
            if (m.name == meshName)
            {
                outImport = m.importTransform;
                return true;
            }
        }
        std::cerr << "[FbxConverter] Scene 에서 mesh-name '" << meshName << "' 없음 (검사 인스턴스 수: "
                  << scene.meshes.size() << ")\n";
        return false;
    }

    // engine::scene::Transform → FbxAMatrix (column-vector convention, FBX 표준).
    // T·R·S 순서로 합성 (XMMatrixAffineTransformation 결과의 column-vector 표현).
    FbxAMatrix ToFbxMatrix(const engine::scene::Transform& xform)
    {
        FbxAMatrix m;
        // SetT / SetQ / SetS — FbxAMatrix 의 affine 표현.
        m.SetT(FbxVector4(xform.position.x, xform.position.y, xform.position.z, 0.0));
        m.SetQ(FbxQuaternion(xform.rotation.x, xform.rotation.y, xform.rotation.z, xform.rotation.w));
        m.SetS(FbxVector4(xform.scale.x, xform.scale.y, xform.scale.z, 0.0));
        return m;
    }

    // FBX scene 의 hierarchy + bone bind pose + animation 을 진단 dump.
    //   *변환 적용 없이* — 우리 FbxLoader 의 mesh-local normalize 동작이 baked-in 을
    //   상쇄하는 구조 확인을 위한 분석 단계.

    int DiagDepth = 0;
    void DiagDumpNode(FbxNode* node)
    {
        if (node == nullptr) { return; }
        const std::string indent(DiagDepth * 2, ' ');
        const char* attrName = "?";
        FbxNodeAttribute* attr = node->GetNodeAttribute();
        if (attr)
        {
            switch (attr->GetAttributeType())
            {
                case FbxNodeAttribute::eSkeleton: attrName = "Skeleton"; break;
                case FbxNodeAttribute::eMesh:     attrName = "Mesh"; break;
                case FbxNodeAttribute::eNull:     attrName = "Null"; break;
                default:                          attrName = "Other"; break;
            }
        }
        const FbxAMatrix gx = node->EvaluateGlobalTransform();
        const FbxVector4 gT = gx.GetT();
        std::cout << indent << "[" << attrName << "] " << node->GetName()
                  << " GlobalT=(" << gT[0] << "," << gT[1] << "," << gT[2] << ")\n";
        ++DiagDepth;
        for (int i = 0; i < node->GetChildCount(); ++i)
        {
            DiagDumpNode(node->GetChild(i));
        }
        --DiagDepth;
    }
}

int main(int argc, char** argv)
{
    Args args;
    if (!ParseArgs(argc, argv, args))
    {
        return 1;
    }

    std::cout << "[FbxConverter] input=" << args.input
              << " output=" << args.output
              << " scene=" << args.scenePath
              << " mesh=" << args.meshName << "\n";

    // 1) Scene JSON 의 importTransform 추출.
    engine::scene::Transform importT;
    try
    {
        if (!FindImportTransform(args.scenePath, args.meshName, importT))
        {
            return 1;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[FbxConverter] Scene JSON 로드 실패: " << e.what() << "\n";
        return 1;
    }
    std::cout << "[FbxConverter] importTransform:"
              << " pos=("  << importT.position.x << "," << importT.position.y << "," << importT.position.z << ")"
              << " rot=("  << importT.rotation.x << "," << importT.rotation.y << "," << importT.rotation.z << "," << importT.rotation.w << ")"
              << " scale=("<< importT.scale.x    << "," << importT.scale.y    << "," << importT.scale.z    << ")\n";

    // identity 면 변환 무의미 — 그래도 그대로 복사 export.
    const FbxAMatrix importM = ToFbxMatrix(importT);

    // 2) FBX 로드.
    FbxManager* manager = FbxManager::Create();
    if (manager == nullptr) { std::cerr << "FbxManager::Create 실패\n"; return 1; }

    FbxIOSettings* ios = FbxIOSettings::Create(manager, IOSROOT);
    manager->SetIOSettings(ios);

    FbxScene*    scene    = FbxScene::Create(manager, "");
    FbxImporter* importer = FbxImporter::Create(manager, "");
    if (!importer->Initialize(args.input.c_str(), -1, manager->GetIOSettings()))
    {
        std::cerr << "[FbxConverter] FbxImporter::Initialize 실패: "
                  << importer->GetStatus().GetErrorString() << "\n";
        manager->Destroy();
        return 1;
    }
    if (!importer->Import(scene))
    {
        std::cerr << "[FbxConverter] FbxImporter::Import 실패\n";
        manager->Destroy();
        return 1;
    }
    importer->Destroy();

    // 3) (보류) 변환 코드 — 우리 FbxLoader 의 mesh-local normalize 패턴이 baked-in 을
    //    상쇄하는 구조라 단순 mesh CP / cluster TM 변환만으로는 mesh skinning 이 깨졌다.
    //    정확한 변환을 위해서는 FbxLoader 측의 mesh-local normalize 우회 또는 별도 좌표계
    //    합의가 필요. 현 단계는 *진단 dump* 만 출력하고 변환은 No-op.
    std::cout << "[FbxConverter] (진단 모드) scene hierarchy dump:\n";
    DiagDumpNode(scene->GetRootNode());
    std::cout << "[FbxConverter] (변환 보류) 입력 그대로 export — 후속 단계에서 정확한 baked-in 구현 필요.\n";

    // 4) FBX export — binary format (가장 호환성 좋은 *FBX 2019* binary).
    FbxExporter* exporter = FbxExporter::Create(manager, "");
    const int binaryFmt = manager->GetIOPluginRegistry()->FindWriterIDByDescription("FBX binary (*.fbx)");
    if (!exporter->Initialize(args.output.c_str(), binaryFmt, manager->GetIOSettings()))
    {
        std::cerr << "[FbxConverter] FbxExporter::Initialize 실패: "
                  << exporter->GetStatus().GetErrorString() << "\n";
        manager->Destroy();
        return 1;
    }
    if (!exporter->Export(scene))
    {
        std::cerr << "[FbxConverter] FbxExporter::Export 실패\n";
        manager->Destroy();
        return 1;
    }
    exporter->Destroy();
    manager->Destroy();

    std::cout << "[FbxConverter] export 완료: " << args.output << "\n";
    return 0;
}
