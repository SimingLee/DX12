"""
Export StaticMeshActor placement data from the currently open Unreal level.

Run inside Unreal Editor's Python console or an Editor Utility script. Set
FBX_ROOT to the directory that contains FBX files named after each StaticMesh
asset if you want the engine importer to resolve meshes automatically.
"""

import json
import os

import unreal


FBX_ROOT = ""
OUTPUT_PATH = os.path.join(unreal.Paths.project_saved_dir(), "dx12_static_mesh_level.json")


def make_fbx_path(static_mesh):
    if not FBX_ROOT or static_mesh is None:
        return ""
    asset_name = static_mesh.get_name()
    return os.path.join(FBX_ROOT, asset_name + ".fbx").replace("\\", "/")


def vector_to_list(value):
    return [float(value.x), float(value.y), float(value.z)]


def rotator_to_list(value):
    return [float(value.roll), float(value.pitch), float(value.yaw)]


def main():
    actors = []
    for actor in unreal.EditorLevelLibrary.get_all_level_actors():
        if not isinstance(actor, unreal.StaticMeshActor):
            continue

        component = actor.static_mesh_component
        static_mesh = component.static_mesh if component else None
        transform = actor.get_actor_transform()

        actors.append(
            {
                "name": actor.get_actor_label(),
                "staticMeshPath": static_mesh.get_path_name() if static_mesh else "",
                "fbxPath": make_fbx_path(static_mesh),
                "location": vector_to_list(transform.translation),
                "rotationEuler": rotator_to_list(transform.rotation.rotator()),
                "scale": vector_to_list(transform.scale3d),
            }
        )

    with open(OUTPUT_PATH, "w", encoding="utf-8") as output:
        json.dump({"actors": actors}, output, indent=2)

    unreal.log("Exported {} StaticMeshActor entries to {}".format(len(actors), OUTPUT_PATH))


if __name__ == "__main__":
    main()
