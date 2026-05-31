"""Unreal Editor Python 스크립트 — Lyra Mannequin + 주요 애니메이션을 FBX 로 일괄 export.

사용법:
  1. Edit → Plugins → "Python Editor Script Plugin" 활성화 (재시작 필요).
  2. Window → Output Log → 하단 입력란 옆 드롭다운 'Cmd' → 'Python' 전환.
  3. 이 파일 내용을 통째로 붙여넣고 Enter.
  4. 콘솔 로그: [export] mesh -> ... / [export] anim -> ... / [export] DONE — ok=5

export 대상 (필요 시 ASSETS 리스트 편집):
  - Skeletal Mesh: SKM_Quinn (Lyra female mannequin)
  - Animations: Idle, Walk, Jog (Unarmed) + Jump (Rifle, takeoff phase)
"""

import os
import unreal

# ----------------------------------------------------------------------------
# 출력 폴더 — OS native 절대 경로.
OUT_DIR = r"e:\Things\portfolio_engine\Resources\FBX\UE"

# 변환 대상. (asset path in UE Content Browser, output file name)
# Lyra Mannequin 기준. 캐릭터/애니메이션 위치 다르면 Copy Reference 결과로 수정.
#
# Quinn (여성). Manny (남성) 으로 변경 시 SKM_Quinn → SKM_Manny.
# Jump 는 Unarmed 폴더에 없어 Rifle Jump_Start 사용 — takeoff phase 만.
ASSETS = [
    # Skeletal Mesh (mesh + skeleton).
    ("/Game/Characters/Heroes/Mannequin/Meshes/SKM_Quinn", "Quinn.fbx"),

    # Locomotion — Unarmed (무기 없는 자세).
    ("/Game/Characters/Heroes/Mannequin/Animations/Locomotion/Unarmed/MM_Unarmed_Idle_Ready", "Quinn_Idle.fbx"),
    ("/Game/Characters/Heroes/Mannequin/Animations/Locomotion/Unarmed/MF_Unarmed_Walk_Fwd",   "Quinn_Walk.fbx"),
    ("/Game/Characters/Heroes/Mannequin/Animations/Locomotion/Unarmed/MF_Unarmed_Jog_Fwd",    "Quinn_Jog.fbx"),

    # Jump — Unarmed 에 jump 없어 Rifle 의 Start phase 사용.
    ("/Game/Characters/Heroes/Mannequin/Animations/Locomotion/Rifle/MM_Rifle_Jump_Start",     "Quinn_Jump.fbx"),
]

# ----------------------------------------------------------------------------

def ensure_out_dir():
    if not os.path.isdir(OUT_DIR):
        os.makedirs(OUT_DIR)
        unreal.log("[export] created: " + OUT_DIR)


def export_skeletal_mesh(asset, out_path):
    """Skeletal Mesh export — mesh + skeleton 포함."""
    task = unreal.AssetExportTask()
    task.object = asset
    task.filename = out_path
    task.automated = True
    task.prompt = False
    task.replace_identical = True

    options = unreal.FbxExportOption()
    options.export_morph_targets = True
    options.export_preview_mesh = True
    options.level_of_detail = False
    task.options = options

    if unreal.Exporter.run_asset_export_task(task):
        unreal.log("[export] mesh -> " + out_path)
        return True
    unreal.log_error("[export] FAILED mesh -> " + out_path)
    return False


def export_anim_sequence(asset, out_path):
    """Animation Sequence export — FBX track 으로 baked."""
    task = unreal.AssetExportTask()
    task.object = asset
    task.filename = out_path
    task.automated = True
    task.prompt = False
    task.replace_identical = True

    options = unreal.FbxExportOption()
    options.export_morph_targets = True
    options.level_of_detail = False
    task.options = options

    if unreal.Exporter.run_asset_export_task(task):
        unreal.log("[export] anim -> " + out_path)
        return True
    unreal.log_error("[export] FAILED anim -> " + out_path)
    return False


def main():
    ensure_out_dir()
    asset_lib = unreal.EditorAssetLibrary

    ok = 0
    fail = 0
    for src_path, out_name in ASSETS:
        if not asset_lib.does_asset_exist(src_path):
            unreal.log_warning("[export] MISSING (path) " + src_path)
            fail += 1
            continue

        asset = asset_lib.load_asset(src_path)
        if asset is None:
            unreal.log_warning("[export] LOAD FAILED " + src_path)
            fail += 1
            continue

        out_path = os.path.join(OUT_DIR, out_name)
        if isinstance(asset, unreal.SkeletalMesh):
            success = export_skeletal_mesh(asset, out_path)
        elif isinstance(asset, unreal.AnimSequence):
            success = export_anim_sequence(asset, out_path)
        else:
            unreal.log_warning("[export] unsupported type " + str(type(asset)) + " for " + src_path)
            fail += 1
            continue

        if success:
            ok += 1
        else:
            fail += 1

    unreal.log("[export] DONE — ok=%d, fail=%d, output=%s" % (ok, fail, OUT_DIR))


main()
