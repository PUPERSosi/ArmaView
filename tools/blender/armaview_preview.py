"""ArmaView: Export & Preview

Blender add-on (4.2 - 5.0) next to "Arma 3 Object Builder" (a3ob, tested against 2.5.1):
exports the model / animation with a3ob's own exporters (bpy.ops.a3ob.export_p3d / export_rtm, using your last
export settings) and hands the file to ArmaView. A running ArmaView window receives it through its single-instance
pipe (same model -> reloaded with camera/bone/animation kept, same RTM -> layer updated); otherwise ArmaView starts.
Hot reload in ArmaView also catches every later save of the same files.

Install: Edit > Preferences > Add-ons > (v) > Install from Disk... > this file, then enable it.
Panel: 3D Viewport > Sidebar (N) > ArmaView.
"""

bl_info = {
    "name": "ArmaView: Export & Preview",
    "author": "Pin",
    "version": (1, 0, 1),
    "blender": (4, 2, 0),
    "location": "View3D > Sidebar > ArmaView",
    "description": "Export with Arma 3 Object Builder and preview the result in ArmaView",
    "category": "Import-Export",
}

import os
import subprocess

import bpy
from bpy.props import BoolProperty, PointerProperty, StringProperty

# no default: set the path in the add-on preferences (Edit > Preferences > Add-ons > ArmaView)
DEFAULT_EXE = ""
EXE_HINT = "Set the path to ArmaView.exe in the add-on preferences (Edit > Preferences > Add-ons > ArmaView)"


def _prefs(context):
    addon = context.preferences.addons.get(__name__)
    return addon.preferences if addon else None


def _exe_path(context):
    prefs = _prefs(context)
    path = prefs.exe_path if prefs and prefs.exe_path else DEFAULT_EXE
    return bpy.path.abspath(path) if path else ""


def _a3ob_ready(name):
    return hasattr(bpy.ops, "a3ob") and hasattr(bpy.ops.a3ob, name)


def _mtime(path):
    try:
        return os.stat(path).st_mtime_ns
    except OSError:
        return None


def _send_to_viewer(operator, context, path):
    exe = _exe_path(context)
    if not exe:
        operator.report({'ERROR'}, EXE_HINT)
        return False
    if not os.path.isfile(exe):
        operator.report({'ERROR'}, "ArmaView.exe not found: %s (set it in the add-on preferences)" % exe)
        return False
    flags = 0x00000008 | 0x00000200  # DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP: Blender does not wait for it
    try:
        subprocess.Popen([exe, path], close_fds=True, creationflags=flags, cwd=os.path.dirname(exe))
    except OSError as e:
        operator.report({'ERROR'}, "Could not start ArmaView: %s" % e)
        return False
    return True


def _target(operator, raw, ext):
    path = bpy.path.abspath(raw) if raw else ""
    if not path:
        operator.report({'ERROR'}, "Set the %s file path in the ArmaView panel" % ext.upper())
        return None
    if not path.lower().endswith(ext):
        path += ext
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    return path


class ARMAVIEW_AddonPreferences(bpy.types.AddonPreferences):
    bl_idname = __name__

    exe_path: StringProperty(name="ArmaView.exe", subtype='FILE_PATH', default=DEFAULT_EXE,
                             description="Set the path to ArmaView.exe")

    def draw(self, context):
        self.layout.prop(self, "exe_path")
        if not self.exe_path:
            self.layout.label(text="Set the path to ArmaView.exe", icon='ERROR')


class ARMAVIEW_SceneProps(bpy.types.PropertyGroup):
    p3d_path: StringProperty(name="P3D", subtype='FILE_PATH', description="Where the model is exported (MLOD P3D)")
    rtm_path: StringProperty(name="RTM", subtype='FILE_PATH', description="Where the animation is exported")
    selected_only: BoolProperty(name="Selected Only", description="Export only the selected LOD objects", default=False)
    open_viewer: BoolProperty(name="Show in ArmaView", description="Send the exported file to ArmaView", default=True)


class ARMAVIEW_OT_export_p3d(bpy.types.Operator):
    """Export the LOD objects with Arma 3 Object Builder and show the P3D in ArmaView"""

    bl_idname = "armaview.export_p3d_preview"
    bl_label = "Export P3D & Preview"
    bl_options = {'REGISTER'}

    def execute(self, context):
        props = context.scene.armaview
        if not _a3ob_ready("export_p3d"):
            self.report({'ERROR'}, "Arma 3 Object Builder is not enabled (bpy.ops.a3ob.export_p3d missing)")
            return {'CANCELLED'}
        path = _target(self, props.p3d_path, ".p3d")
        if not path:
            return {'CANCELLED'}
        before = _mtime(path)
        try:
            result = bpy.ops.a3ob.export_p3d('EXEC_DEFAULT', filepath=path, use_selection=props.selected_only)
        except RuntimeError as e:
            self.report({'ERROR'}, "a3ob export failed: %s" % e)
            return {'CANCELLED'}
        if 'FINISHED' not in result or _mtime(path) in (None, before):
            self.report({'ERROR'}, "a3ob did not write %s (see its message / the system console)" % path)
            return {'CANCELLED'}
        if props.open_viewer:
            _send_to_viewer(self, context, path)
        self.report({'INFO'}, "Exported %s" % path)
        return {'FINISHED'}


class ARMAVIEW_OT_export_rtm(bpy.types.Operator):
    """Export the active armature's action with Arma 3 Object Builder and play it in ArmaView"""

    bl_idname = "armaview.export_rtm_preview"
    bl_label = "Export RTM & Preview"
    bl_options = {'REGISTER'}

    @classmethod
    def poll(cls, context):
        obj = context.object
        return obj is not None and obj.type == 'ARMATURE'

    def execute(self, context):
        props = context.scene.armaview
        if not _a3ob_ready("export_rtm"):
            self.report({'ERROR'}, "Arma 3 Object Builder is not enabled (bpy.ops.a3ob.export_rtm missing)")
            return {'CANCELLED'}
        rig = getattr(context.scene, "a3ob_rigging", None)
        if rig is None or len(rig.skeletons) == 0:
            self.report({'ERROR'}, "Define a skeleton in a3ob's Rigging panel first (RTM export needs it)")
            return {'CANCELLED'}
        path = _target(self, props.rtm_path, ".rtm")
        if not path:
            return {'CANCELLED'}
        before = _mtime(path)
        scene = context.scene
        try:
            # Frame range from the scene; the rest (source, step, skeleton) are a3ob's last-used export settings.
            result = bpy.ops.a3ob.export_rtm('EXEC_DEFAULT', filepath=path,
                                             frame_start=scene.frame_start, frame_end=scene.frame_end)
        except RuntimeError as e:
            self.report({'ERROR'}, "a3ob RTM export failed: %s" % e)
            return {'CANCELLED'}
        if 'FINISHED' not in result or _mtime(path) in (None, before):
            self.report({'ERROR'}, "a3ob did not write %s (check the skeleton / its message)" % path)
            return {'CANCELLED'}
        if props.open_viewer:
            _send_to_viewer(self, context, path)
        self.report({'INFO'}, "Exported %s" % path)
        return {'FINISHED'}


class ARMAVIEW_OT_open(bpy.types.Operator):
    """Open the already exported file in ArmaView without exporting"""

    bl_idname = "armaview.open"
    bl_label = "Open in ArmaView"

    which: StringProperty(default="p3d", options={'HIDDEN'})

    def execute(self, context):
        props = context.scene.armaview
        raw = props.p3d_path if self.which == "p3d" else props.rtm_path
        path = bpy.path.abspath(raw) if raw else ""
        if not path or not os.path.isfile(path):
            self.report({'ERROR'}, "File not found: %s" % (path or "(empty path)"))
            return {'CANCELLED'}
        return {'FINISHED'} if _send_to_viewer(self, context, path) else {'CANCELLED'}


class ARMAVIEW_PT_panel(bpy.types.Panel):
    bl_label = "ArmaView"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "ArmaView"

    def draw(self, context):
        layout = self.layout
        props = context.scene.armaview
        if not _a3ob_ready("export_p3d"):
            layout.label(text="Arma 3 Object Builder is not enabled", icon='ERROR')

        box = layout.box()
        box.label(text="Model", icon='MESH_DATA')
        box.prop(props, "p3d_path", text="")
        box.prop(props, "selected_only")
        row = box.row(align=True)
        row.scale_y = 1.4
        row.operator(ARMAVIEW_OT_export_p3d.bl_idname, icon='EXPORT')
        row.operator(ARMAVIEW_OT_open.bl_idname, text="", icon='HIDE_OFF').which = "p3d"

        box = layout.box()
        box.label(text="Animation", icon='ARMATURE_DATA')
        box.prop(props, "rtm_path", text="")
        row = box.row(align=True)
        row.scale_y = 1.4
        row.operator(ARMAVIEW_OT_export_rtm.bl_idname, icon='EXPORT')
        row.operator(ARMAVIEW_OT_open.bl_idname, text="", icon='HIDE_OFF').which = "rtm"
        if context.object is None or context.object.type != 'ARMATURE':
            box.label(text="Select the armature to export RTM", icon='INFO')

        layout.prop(props, "open_viewer")


classes = (
    ARMAVIEW_AddonPreferences,
    ARMAVIEW_SceneProps,
    ARMAVIEW_OT_export_p3d,
    ARMAVIEW_OT_export_rtm,
    ARMAVIEW_OT_open,
    ARMAVIEW_PT_panel,
)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    bpy.types.Scene.armaview = PointerProperty(type=ARMAVIEW_SceneProps)


def unregister():
    del bpy.types.Scene.armaview
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)


if __name__ == "__main__":
    register()
