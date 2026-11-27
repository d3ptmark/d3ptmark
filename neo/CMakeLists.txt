cmake_minimum_required(VERSION 3.18...3.30 FATAL_ERROR)
project(d3ptmark C CXX)

# d3ptmark - Doom 3 Path-Tracer Benchmark (based on dhewm3).
# Builds two engine binaries from the same source tree:
#   d3ptmark-le - legacy OpenGL renderer
#   d3ptmark-pt - GPU path tracer (compiled with -DPT_PATHTRACED), CUDA and/or OpenCL backends
# plus the game libraries (base, d3xp) shared by both binaries.
# Supported platforms: Linux (GCC/Clang/MinGW) and Windows (MSVC).

option(LEGACY     "Build the legacy renderer binary (d3ptmark-le)" ON)
option(PATHTRACED "Build the path-traced binary (d3ptmark-pt), requires the NVIDIA CUDA toolkit and/or the OpenCL headers" ON)
option(OPENCL     "Add the OpenCL 1.2 backend to d3ptmark-pt (needs only the OpenCL headers, the runtime is loaded on demand)" ON)
option(TESTS      "Build the path tracer regression tests (run with ctest)" OFF)

set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/sys/cmake")

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE "RelWithDebInfo")
endif()

# the engine code is C++11 (required by Dear ImGui); CUDA kernels are C++17
set(CMAKE_CXX_STANDARD 11)

# precompiled dependencies from the dhewm3-libs repo (mainly for Windows)
if(DHEWM3LIBS)
	if(CMAKE_CROSSCOMPILING)
		set(CMAKE_FIND_ROOT_PATH ${DHEWM3LIBS})
	else()
		set(ENV{CMAKE_PREFIX_PATH} ${DHEWM3LIBS})
	endif()
	set(ENV{OPENALDIR} ${DHEWM3LIBS})
	set(ENV{SDL2DIR} ${DHEWM3LIBS})
endif()

# ---------------------------------------------------------------------------
# dependencies
# ---------------------------------------------------------------------------

find_package(OpenAL REQUIRED)
include_directories(${OPENAL_INCLUDE_DIR})

if(WIN32)
	set(SDL2_BUILDING_LIBRARY TRUE) # skip SDL2main, the engine has its own WinMain
endif()
find_package(SDL2 REQUIRED)
include_directories(${SDL2_INCLUDE_DIR})

find_package(CURL QUIET)
if(CURL_FOUND)
	set(ID_ENABLE_CURL ON)
	include_directories(${CURL_INCLUDE_DIRS})
	set(curl_lib ${CURL_LIBRARIES})
else()
	message(STATUS "libcurl not found, server downloads won't be available")
	set(ID_ENABLE_CURL OFF)
	set(curl_lib "")
endif()

set(PT_HAVE_CUDA OFF)
set(PT_HAVE_OPENCL OFF)
if(PATHTRACED)
	# OpenCL backend: only the headers are needed at build time, the ICD loader
	# (OpenCL.dll / libOpenCL.so.1) is loaded when the backend starts
	if(OPENCL)
		find_package(OpenCL QUIET)
		if(OpenCL_INCLUDE_DIRS)
			set(PT_OPENCL_INCLUDE_DIR ${OpenCL_INCLUDE_DIRS})
		else()
			find_path(PT_OPENCL_INCLUDE_DIR NAMES CL/cl.h
				HINTS ENV OCL_ROOT ENV CUDA_PATH ENV INTELOCLSDKROOT ENV AMDAPPSDKROOT PATH_SUFFIXES include)
		endif()
		if(PT_OPENCL_INCLUDE_DIR)
			set(PT_HAVE_OPENCL ON)
			message(STATUS "OpenCL path tracer enabled - headers: ${PT_OPENCL_INCLUDE_DIR}")
		else()
			message(WARNING "OpenCL headers (CL/cl.h) not found, d3ptmark-pt is built without the OpenCL backend")
		endif()
	endif()

	include(CheckLanguage)
	check_language(CUDA)
	if(CMAKE_CUDA_COMPILER)
		set(PT_HAVE_CUDA ON)
		enable_language(CUDA)
		set(CMAKE_CUDA_STANDARD 17)
		if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES OR CMAKE_CUDA_ARCHITECTURES STREQUAL "")
			set(CMAKE_CUDA_ARCHITECTURES "75;80;86;89;90") # Turing..Hopper
		endif()
		message(STATUS "CUDA path tracer enabled - compiler: ${CMAKE_CUDA_COMPILER}, architectures: ${CMAKE_CUDA_ARCHITECTURES}")
	elseif(PT_HAVE_OPENCL)
		message(STATUS "NVIDIA CUDA toolkit not found, d3ptmark-pt is built with the OpenCL backend only")
	else()
		message(FATAL_ERROR "d3ptmark-pt needs the NVIDIA CUDA toolkit or the OpenCL headers - install one or configure with -DPATHTRACED=OFF")
	endif()
endif()

# ---------------------------------------------------------------------------
# target OS / CPU defines
# ---------------------------------------------------------------------------

string(TOLOWER "${CMAKE_SYSTEM_NAME}" os)
# all supported targets are little endian
add_definitions(-DD3_OSTYPE="${os}" -DD3_SIZEOFPTR=${CMAKE_SIZEOF_VOID_P} -DD3_IS_BIG_ENDIAN=0)

if(NOT MSVC) # for MSVC, D3_ARCH is set in neo/sys/platform.h
	set(cpu ${CMAKE_SYSTEM_PROCESSOR})
	if(cpu MATCHES "i.86")
		set(cpu "x86")
	elseif(cpu MATCHES "[aA][mM][dD]64|[xX]64")
		set(cpu "x86_64")
	elseif(cpu MATCHES "aarch64|arm64")
		set(cpu "arm64")
	endif()
	add_definitions(-DD3_ARCH="${cpu}")
	message(STATUS "Building ${CMAKE_BUILD_TYPE} for ${os}-${cpu}")
endif()

# ---------------------------------------------------------------------------
# compiler flags
# ---------------------------------------------------------------------------

set(ldflags "")
set(sys_libs "")

if(MSVC)
	# /MP: parallel build, /we464x: treat some truncating conversions as errors
	add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/MP;/W3;/we4840;/we4306;/we4311;/we4312;/we4302>")
	# ignore warnings about possible loss of data, deprecated functions etc.
	add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/wd4100;/wd4127;/wd4244;/wd4245;/wd4267;/wd4714;/wd4996;/wd4068;/wd4458>")
	add_definitions(-D_ALLOW_KEYWORD_MACROS) # for the "#define private public" in TypeInfo.cpp
	set(CMAKE_C_FLAGS_DEBUG "-D_DEBUG /Od /Zi /MDd")
	set(CMAKE_C_FLAGS_RELEASE "/Ox /Oy /MD")
	set(CMAKE_C_FLAGS_RELWITHDEBINFO "/Ox /Oy /Zi /MD")
	set(CMAKE_C_FLAGS_MINSIZEREL "/Ox /Oy /Os /MD")
	# 8MB stack instead of 1MB, so big models don't overflow it with _alloca()
	set(ldflags "${ldflags} /STACK:8388608")
else() # GCC/Clang (hopefully)
	add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-pipe;-Wall;-fno-strict-aliasing>")
	# don't let FMA "optimizations" change math results
	add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-ffp-contract=off>")
	# TODO: fix the code instead of suppressing the warnings
	add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-Wno-sign-compare;-Wno-switch;-Wno-class-memaccess>")
	if(NOT WIN32)
		add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-fvisibility=hidden>")
	endif()

	set(CMAKE_C_FLAGS_DEBUG "-g -ggdb -D_DEBUG -O0")
	set(CMAKE_C_FLAGS_RELEASE "-O2 -fno-math-errno -fno-trapping-math -ffinite-math-only -fomit-frame-pointer")
	set(CMAKE_C_FLAGS_RELWITHDEBINFO "-g -ggdb -O2 -fno-math-errno -fno-trapping-math -ffinite-math-only -fno-omit-frame-pointer")
	set(CMAKE_C_FLAGS_MINSIZEREL "-Os -fno-math-errno -fno-trapping-math -ffinite-math-only -fomit-frame-pointer")

	if(WIN32) # MinGW: static runtime and 8MB stack
		set(ldflags "${ldflags} -static-libgcc -static-libstdc++ -Wl,--stack,8388608")
	elseif(os STREQUAL "linux")
		set(sys_libs ${sys_libs} dl)
	endif()
endif()

set(CMAKE_CXX_FLAGS_DEBUG ${CMAKE_C_FLAGS_DEBUG})
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO ${CMAKE_C_FLAGS_RELWITHDEBINFO})
# disable assertions in release builds
set(CMAKE_CXX_FLAGS_RELEASE "-DNDEBUG ${CMAKE_C_FLAGS_RELEASE}")
set(CMAKE_CXX_FLAGS_MINSIZEREL "-DNDEBUG ${CMAKE_C_FLAGS_MINSIZEREL}")

if(WIN32)
	add_definitions(-DWINVER=0x0501 -D_WIN32_WINNT=0x0501)
	set(sys_libs ${sys_libs} winmm iphlpapi wsock32 ole32)
endif()

# optimize integrated third-party sources (stb_*, miniz, ...) even in debug builds
function(always_optimize_sourcefile srcfilename)
	if(MSVC)
		set_source_files_properties(${srcfilename} PROPERTIES COMPILE_OPTIONS "/Ox")
	else()
		set_source_files_properties(${srcfilename} PROPERTIES COMPILE_OPTIONS "-O2")
	endif()
endfunction()

# ---------------------------------------------------------------------------
# config.h and build revision
# ---------------------------------------------------------------------------

set(libdir  "${CMAKE_INSTALL_PREFIX}/lib/d3ptmark")
set(datadir "${CMAKE_INSTALL_PREFIX}/share/d3ptmark")

configure_file("${CMAKE_SOURCE_DIR}/config.h.in" "${CMAKE_BINARY_DIR}/config.h")

execute_process(COMMAND git rev-parse --short=12 HEAD WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
	OUTPUT_VARIABLE PT_CAPTURE_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT PT_CAPTURE_REVISION)
	set(PT_CAPTURE_REVISION "unknown")
else()
	execute_process(COMMAND git diff --quiet HEAD WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
		RESULT_VARIABLE PT_CAPTURE_DIRTY ERROR_QUIET)
	if(PT_CAPTURE_DIRTY)
		string(APPEND PT_CAPTURE_REVISION "-dirty")
	endif()
endif()

include_directories(${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR})

# ---------------------------------------------------------------------------
# sources
# ---------------------------------------------------------------------------

set(src_idlib
	idlib/bv/Bounds.cpp
	idlib/bv/Frustum.cpp
	idlib/bv/Sphere.cpp
	idlib/bv/Box.cpp
	idlib/geometry/DrawVert.cpp
	idlib/geometry/Winding2D.cpp
	idlib/geometry/Surface_SweptSpline.cpp
	idlib/geometry/Winding.cpp
	idlib/geometry/Surface.cpp
	idlib/geometry/Surface_Patch.cpp
	idlib/geometry/TraceModel.cpp
	idlib/geometry/JointTransform.cpp
	idlib/hashing/CRC32.cpp
	idlib/hashing/MD4.cpp
	idlib/hashing/MD5.cpp
	idlib/math/Angles.cpp
	idlib/math/Lcp.cpp
	idlib/math/Math.cpp
	idlib/math/Matrix.cpp
	idlib/math/Ode.cpp
	idlib/math/Plane.cpp
	idlib/math/Pluecker.cpp
	idlib/math/Polynomial.cpp
	idlib/math/Quat.cpp
	idlib/math/Rotation.cpp
	idlib/math/Simd.cpp
	idlib/math/Simd_Generic.cpp
	idlib/math/Simd_AltiVec.cpp
	idlib/math/Simd_MMX.cpp
	idlib/math/Simd_3DNow.cpp
	idlib/math/Simd_SSE.cpp
	idlib/math/Simd_SSE2.cpp
	idlib/math/Simd_SSE3.cpp
	idlib/math/Vector.cpp
	idlib/BitMsg.cpp
	idlib/LangDict.cpp
	idlib/Lexer.cpp
	idlib/Lib.cpp
	idlib/containers/HashIndex.cpp
	idlib/Dict.cpp
	idlib/Str.cpp
	idlib/Parser.cpp
	idlib/MapFile.cpp
	idlib/CmdArgs.cpp
	idlib/Token.cpp
	idlib/Base64.cpp
	idlib/Timer.cpp
	idlib/Heap.cpp
)

set(src_renderer
	renderer/Cinematic.cpp
	renderer/GuiModel.cpp
	renderer/Image_files.cpp
	renderer/Image_init.cpp
	renderer/Image_load.cpp
	renderer/Image_process.cpp
	renderer/Image_program.cpp
	renderer/Interaction.cpp
	renderer/Material.cpp
	renderer/MegaTexture.cpp
	renderer/Model.cpp
	renderer/ModelDecal.cpp
	renderer/ModelManager.cpp
	renderer/ModelOverlay.cpp
	renderer/Model_beam.cpp
	renderer/Model_ase.cpp
	renderer/Model_liquid.cpp
	renderer/Model_lwo.cpp
	renderer/Model_ma.cpp
	renderer/Model_md3.cpp
	renderer/Model_md5.cpp
	renderer/Model_prt.cpp
	renderer/Model_sprite.cpp
	renderer/RenderEntity.cpp
	renderer/RenderSystem.cpp
	renderer/RenderSystem_init.cpp
	renderer/RenderWorld.cpp
	renderer/RenderWorld_demo.cpp
	renderer/RenderWorld_load.cpp
	renderer/RenderWorld_portals.cpp
	renderer/VertexCache.cpp
	renderer/draw_arb2.cpp
	renderer/draw_common.cpp
	renderer/tr_backend.cpp
	renderer/tr_deform.cpp
	renderer/tr_font.cpp
	renderer/tr_guisurf.cpp
	renderer/tr_light.cpp
	renderer/tr_lightrun.cpp
	renderer/tr_main.cpp
	renderer/tr_orderIndexes.cpp
	renderer/tr_polytope.cpp
	renderer/tr_render.cpp
	renderer/tr_render_gl_stats.cpp
	renderer/tr_rendertools.cpp
	renderer/tr_shadowbounds.cpp
	renderer/tr_stencilshadow.cpp
	renderer/tr_subview.cpp
	renderer/tr_trace.cpp
	renderer/tr_trisurf.cpp
	renderer/tr_turboshadow.cpp
	renderer/stblib_impls.c
	# benchmark/capture report code is active in both binaries (GL benchmarks)
	renderer_pt/pt_benchmark.cpp
	renderer_pt/pt_capture_report.cpp
	renderer_pt/pt_gpu_telemetry.cpp
)

always_optimize_sourcefile(renderer/stblib_impls.c)

# path tracer host sources, only compiled into d3ptmark-pt
set(src_pt_host
	renderer_pt/pt_render.cpp
	renderer_pt/pt_ui.cpp
	renderer_pt/pt_commands.cpp
	renderer_pt/pt_scene.cpp
	renderer_pt/pt_scene_material.cpp
	renderer_pt/pt_physical.cpp
	renderer_pt/pt_material_editor.cpp
)

# CUDA backend: host side + device sources (kernels + GPU LBVH builder)
set(src_pt_cuda
	renderer_pt/cuda/pt_backend_cuda.cpp
	renderer_pt/cuda/pt_cuda_kernels.cu
	renderer_pt/cuda/pt_cuda_lbvh.cu
)

# OpenCL backend. The OpenCL C program (pt_types.h, pt_opencl_shared.h and the
# pt_cl_*.cl files in dependency order) is embedded into the executable.
set(src_pt_opencl_program
	renderer_pt/pt_types.h
	renderer_pt/opencl/pt_opencl_shared.h
	renderer_pt/opencl/pt_cl_math.cl
	renderer_pt/opencl/pt_cl_physical.cl
	renderer_pt/opencl/pt_cl_material.cl
	renderer_pt/opencl/pt_cl_triangles.cl
	renderer_pt/opencl/pt_cl_intersect.cl
	renderer_pt/opencl/pt_cl_sampling.cl
	renderer_pt/opencl/pt_cl_pathtracing.cl
	renderer_pt/opencl/pt_cl_denoising.cl
	renderer_pt/opencl/pt_cl_postprocess.cl
	renderer_pt/opencl/pt_cl_lbvh.cl
)
set(src_pt_opencl_launch "")
set(src_pt_opencl "")
if(PT_HAVE_OPENCL)
	set(pt_opencl_header "${CMAKE_BINARY_DIR}/pt_opencl_program.h")
	set(pt_opencl_inputs "")
	foreach(pt_opencl_file ${src_pt_opencl_program})
		list(APPEND pt_opencl_inputs "${CMAKE_SOURCE_DIR}/${pt_opencl_file}")
	endforeach()
	string(REPLACE ";" "|" pt_opencl_inputs_arg "${pt_opencl_inputs}")
	add_custom_command(
		OUTPUT "${pt_opencl_header}"
		COMMAND ${CMAKE_COMMAND} "-DOUTPUT=${pt_opencl_header}" "-DINPUTS=${pt_opencl_inputs_arg}"
			-P "${CMAKE_SOURCE_DIR}/renderer_pt/opencl/pt_opencl_embed.cmake"
		DEPENDS ${pt_opencl_inputs} "${CMAKE_SOURCE_DIR}/renderer_pt/opencl/pt_opencl_embed.cmake"
		COMMENT "Embedding OpenCL path tracer program"
		VERBATIM
	)
	# launch layer without idlib dependencies, shared with the OpenCL regression tests
	set(src_pt_opencl_launch
		renderer_pt/opencl/pt_opencl_kernels.cpp
		renderer_pt/opencl/pt_opencl_loader.cpp
		"${pt_opencl_header}"
	)
	set(src_pt_opencl renderer_pt/opencl/pt_backend_opencl.cpp ${src_pt_opencl_launch})
endif()

set(src_framework
	framework/CVarSystem.cpp
	framework/CmdSystem.cpp
	framework/Common.cpp
	framework/Compressor.cpp
	framework/Console.cpp
	framework/DemoFile.cpp
	framework/DeclAF.cpp
	framework/DeclEntityDef.cpp
	framework/DeclFX.cpp
	framework/DeclManager.cpp
	framework/DeclParticle.cpp
	framework/DeclPDA.cpp
	framework/DeclSkin.cpp
	framework/DeclTable.cpp
	framework/Dhewm3SettingsMenu.cpp
	framework/EditField.cpp
	framework/EventLoop.cpp
	framework/File.cpp
	framework/FileSystem.cpp
	framework/KeyInput.cpp
	framework/UsercmdGen.cpp
	framework/Session_menu.cpp
	framework/Session.cpp
	framework/async/AsyncClient.cpp
	framework/async/AsyncNetwork.cpp
	framework/async/AsyncServer.cpp
	framework/async/MsgChannel.cpp
	framework/async/NetworkSystem.cpp
	framework/async/ServerScan.cpp
	framework/miniz/miniz.c
	framework/minizip/ioapi.c
	framework/minizip/unzip.cpp
)

always_optimize_sourcefile(framework/miniz/miniz.c)
always_optimize_sourcefile(framework/minizip/ioapi.c)
always_optimize_sourcefile(framework/minizip/unzip.cpp)

set(src_cm
	cm/CollisionModel_contacts.cpp
	cm/CollisionModel_contents.cpp
	cm/CollisionModel_debug.cpp
	cm/CollisionModel_files.cpp
	cm/CollisionModel_load.cpp
	cm/CollisionModel_rotate.cpp
	cm/CollisionModel_trace.cpp
	cm/CollisionModel_translate.cpp
)

set(src_tools_compilers
	tools/compilers/dmap/dmap.cpp
	tools/compilers/dmap/facebsp.cpp
	tools/compilers/dmap/gldraw.cpp
	tools/compilers/dmap/glfile.cpp
	tools/compilers/dmap/leakfile.cpp
	tools/compilers/dmap/map.cpp
	tools/compilers/dmap/optimize.cpp
	tools/compilers/dmap/output.cpp
	tools/compilers/dmap/portals.cpp
	tools/compilers/dmap/shadowopt3.cpp
	tools/compilers/dmap/tritjunction.cpp
	tools/compilers/dmap/tritools.cpp
	tools/compilers/dmap/ubrush.cpp
	tools/compilers/dmap/usurface.cpp
	tools/compilers/aas/AASBuild.cpp
	tools/compilers/aas/AASBuild_file.cpp
	tools/compilers/aas/AASBuild_gravity.cpp
	tools/compilers/aas/AASBuild_ledge.cpp
	tools/compilers/aas/AASBuild_merge.cpp
	tools/compilers/aas/AASCluster.cpp
	tools/compilers/aas/AASFile.cpp
	tools/compilers/aas/AASFile_optimize.cpp
	tools/compilers/aas/AASFile_sample.cpp
	tools/compilers/aas/AASReach.cpp
	tools/compilers/aas/AASFileManager.cpp
	tools/compilers/aas/Brush.cpp
	tools/compilers/aas/BrushBSP.cpp
	tools/compilers/roqvq/NSBitmapImageRep.cpp
	tools/compilers/roqvq/codec.cpp
	tools/compilers/roqvq/roq.cpp
	tools/compilers/roqvq/roqParam.cpp
	tools/compilers/renderbump/renderbump.cpp
)

set(src_snd
	sound/snd_cache.cpp
	sound/snd_decoder.cpp
	sound/snd_efxfile.cpp
	sound/snd_emitter.cpp
	sound/snd_shader.cpp
	sound/snd_system.cpp
	sound/snd_wavefile.cpp
	sound/snd_world.cpp
	sound/stbvorbis_impl.c
)

always_optimize_sourcefile(sound/stbvorbis_impl.c)

set(src_ui
	ui/BindWindow.cpp
	ui/ChoiceWindow.cpp
	ui/DeviceContext.cpp
	ui/EditWindow.cpp
	ui/FieldWindow.cpp
	ui/GameBearShootWindow.cpp
	ui/GameBustOutWindow.cpp
	ui/GameSSDWindow.cpp
	ui/GuiScript.cpp
	ui/ListGUI.cpp
	ui/ListWindow.cpp
	ui/MarkerWindow.cpp
	ui/RegExp.cpp
	ui/RenderWindow.cpp
	ui/SimpleWindow.cpp
	ui/SliderWindow.cpp
	ui/UserInterface.cpp
	ui/Window.cpp
	ui/Winvar.cpp
)

set(src_imgui
	libs/imgui/backends/imgui_impl_sdl2.cpp
	libs/imgui/backends/imgui_impl_opengl2.cpp
	libs/imgui/imgui.cpp
	libs/imgui/imgui_draw.cpp
	libs/imgui/imgui_tables.cpp
	libs/imgui/imgui_widgets.cpp
	libs/imgui/imgui_demo.cpp
	sys/sys_imgui.cpp
	sys/imgui_savestyle.cpp
)

set(src_editor_stubs
	tools/edit_stub.cpp
	tools/debugger/DebuggerBreakpoint.cpp
	tools/debugger/DebuggerServer.cpp
	tools/debugger/DebuggerScript.cpp
	tools/debugger/debugger.cpp
)

if(WIN32)
	set(src_sys
		sys/cpu.cpp
		sys/threads.cpp
		sys/events.cpp
		sys/sys_local.cpp
		sys/glimp.cpp
		sys/win32/win_input.cpp
		sys/win32/win_main.cpp
		sys/win32/win_net.cpp
		sys/win32/win_shared.cpp
		sys/win32/win_syscon.cpp
		sys/win32/rc/dhewm3.rc
	)
else()
	set(src_sys
		sys/cpu.cpp
		sys/threads.cpp
		sys/events.cpp
		sys/sys_local.cpp
		sys/glimp.cpp
		sys/posix/posix_net.cpp
		sys/posix/posix_main.cpp
		sys/linux/main.cpp
	)
endif()

set(src_engine
	${src_renderer}
	${src_framework}
	${src_cm}
	${src_tools_compilers}
	${src_snd}
	${src_ui}
	${src_imgui}
	${src_editor_stubs}
	${src_sys}
)

set(src_game
	game/AF.cpp
	game/AFEntity.cpp
	game/Actor.cpp
	game/Camera.cpp
	game/Entity.cpp
	game/BrittleFracture.cpp
	game/Fx.cpp
	game/GameEdit.cpp
	game/Game_local.cpp
	game/Game_network.cpp
	game/Item.cpp
	game/IK.cpp
	game/Light.cpp
	game/Misc.cpp
	game/Mover.cpp
	game/Moveable.cpp
	game/MultiplayerGame.cpp
	game/Player.cpp
	game/PlayerIcon.cpp
	game/PlayerView.cpp
	game/Projectile.cpp
	game/Pvs.cpp
	game/SecurityCamera.cpp
	game/SmokeParticles.cpp
	game/Sound.cpp
	game/Target.cpp
	game/Trigger.cpp
	game/Weapon.cpp
	game/WorldSpawn.cpp
	game/ai/AAS.cpp
	game/ai/AAS_debug.cpp
	game/ai/AAS_pathing.cpp
	game/ai/AAS_routing.cpp
	game/ai/AI.cpp
	game/ai/AI_events.cpp
	game/ai/AI_pathing.cpp
	game/ai/AI_Vagary.cpp
	game/gamesys/DebugGraph.cpp
	game/gamesys/Class.cpp
	game/gamesys/Event.cpp
	game/gamesys/SaveGame.cpp
	game/gamesys/SysCmds.cpp
	game/gamesys/SysCvar.cpp
	game/gamesys/TypeInfo.cpp
	game/anim/Anim.cpp
	game/anim/Anim_Blend.cpp
	game/anim/Anim_Import.cpp
	game/anim/Anim_Testmodel.cpp
	game/script/Script_Compiler.cpp
	game/script/Script_Interpreter.cpp
	game/script/Script_Program.cpp
	game/script/Script_Thread.cpp
	game/physics/Clip.cpp
	game/physics/Force.cpp
	game/physics/Force_Constant.cpp
	game/physics/Force_Drag.cpp
	game/physics/Force_Field.cpp
	game/physics/Force_Spring.cpp
	game/physics/Physics.cpp
	game/physics/Physics_AF.cpp
	game/physics/Physics_Actor.cpp
	game/physics/Physics_Base.cpp
	game/physics/Physics_Monster.cpp
	game/physics/Physics_Parametric.cpp
	game/physics/Physics_Player.cpp
	game/physics/Physics_RigidBody.cpp
	game/physics/Physics_Static.cpp
	game/physics/Physics_StaticMulti.cpp
	game/physics/Push.cpp
)

set(src_d3xp
	d3xp/AF.cpp
	d3xp/AFEntity.cpp
	d3xp/Actor.cpp
	d3xp/Camera.cpp
	d3xp/Entity.cpp
	d3xp/BrittleFracture.cpp
	d3xp/Fx.cpp
	d3xp/GameEdit.cpp
	d3xp/Game_local.cpp
	d3xp/Game_network.cpp
	d3xp/Item.cpp
	d3xp/IK.cpp
	d3xp/Light.cpp
	d3xp/Misc.cpp
	d3xp/Mover.cpp
	d3xp/Moveable.cpp
	d3xp/MultiplayerGame.cpp
	d3xp/Player.cpp
	d3xp/PlayerIcon.cpp
	d3xp/PlayerView.cpp
	d3xp/Projectile.cpp
	d3xp/Pvs.cpp
	d3xp/SecurityCamera.cpp
	d3xp/SmokeParticles.cpp
	d3xp/Sound.cpp
	d3xp/Target.cpp
	d3xp/Trigger.cpp
	d3xp/Weapon.cpp
	d3xp/WorldSpawn.cpp
	d3xp/ai/AAS.cpp
	d3xp/ai/AAS_debug.cpp
	d3xp/ai/AAS_pathing.cpp
	d3xp/ai/AAS_routing.cpp
	d3xp/ai/AI.cpp
	d3xp/ai/AI_events.cpp
	d3xp/ai/AI_pathing.cpp
	d3xp/ai/AI_Vagary.cpp
	d3xp/gamesys/DebugGraph.cpp
	d3xp/gamesys/Class.cpp
	d3xp/gamesys/Event.cpp
	d3xp/gamesys/SaveGame.cpp
	d3xp/gamesys/SysCmds.cpp
	d3xp/gamesys/SysCvar.cpp
	d3xp/gamesys/TypeInfo.cpp
	d3xp/anim/Anim.cpp
	d3xp/anim/Anim_Blend.cpp
	d3xp/anim/Anim_Import.cpp
	d3xp/anim/Anim_Testmodel.cpp
	d3xp/script/Script_Compiler.cpp
	d3xp/script/Script_Interpreter.cpp
	d3xp/script/Script_Program.cpp
	d3xp/script/Script_Thread.cpp
	d3xp/physics/Clip.cpp
	d3xp/physics/Force.cpp
	d3xp/physics/Force_Constant.cpp
	d3xp/physics/Force_Drag.cpp
	d3xp/physics/Force_Field.cpp
	d3xp/physics/Force_Spring.cpp
	d3xp/physics/Physics.cpp
	d3xp/physics/Physics_AF.cpp
	d3xp/physics/Physics_Actor.cpp
	d3xp/physics/Physics_Base.cpp
	d3xp/physics/Physics_Monster.cpp
	d3xp/physics/Physics_Parametric.cpp
	d3xp/physics/Physics_Player.cpp
	d3xp/physics/Physics_RigidBody.cpp
	d3xp/physics/Physics_Static.cpp
	d3xp/physics/Physics_StaticMulti.cpp
	d3xp/physics/Push.cpp
	d3xp/Grabber.cpp
	d3xp/physics/Force_Grab.cpp
)

# ---------------------------------------------------------------------------
# targets
# ---------------------------------------------------------------------------

add_library(idlib STATIC ${src_idlib})
set_property(TARGET idlib PROPERTY POSITION_INDEPENDENT_CODE ON) # linked into the game .so/.dll

# engine executable; pathtraced=TRUE additionally compiles the path tracer
# with every available backend (CUDA and/or OpenCL)
function(add_engine_binary name pathtraced)
	set(src ${src_engine})
	if(pathtraced)
		list(APPEND src ${src_pt_host})
		if(PT_HAVE_CUDA)
			list(APPEND src ${src_pt_cuda})
		endif()
		if(PT_HAVE_OPENCL)
			list(APPEND src ${src_pt_opencl})
		endif()
	endif()

	add_executable(${name} WIN32 ${src})
	target_include_directories(${name} PRIVATE "${CMAKE_SOURCE_DIR}/libs/imgui")
	target_compile_definitions(${name} PRIVATE __DOOM_DLL__ PT_CAPTURE_REVISION="${PT_CAPTURE_REVISION}")
	set_target_properties(${name} PROPERTIES LINK_FLAGS "${ldflags}")
	target_link_libraries(${name} idlib ${OPENAL_LIBRARY} ${curl_lib} ${SDL2_LIBRARY} ${sys_libs})

	if(pathtraced)
		target_compile_definitions(${name} PRIVATE PT_PATHTRACED HAVE_PATHTRACER PT_PHYSICAL)
		if(PT_HAVE_CUDA)
			target_compile_definitions(${name} PRIVATE HAVE_CUDA)
			set_target_properties(${name} PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
			target_include_directories(${name} PRIVATE ${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES})
		endif()
		if(PT_HAVE_OPENCL)
			target_compile_definitions(${name} PRIVATE HAVE_OPENCL CL_TARGET_OPENCL_VERSION=120)
			target_include_directories(${name} PRIVATE ${PT_OPENCL_INCLUDE_DIR})
			target_link_libraries(${name} ${CMAKE_DL_LIBS})
		endif()
	endif()
	# GPU telemetry (pt_gpu_telemetry.cpp) loads NVML / pdh / dxgi at runtime
	target_link_libraries(${name} ${CMAKE_DL_LIBS})
endfunction()

if(LEGACY)
	add_engine_binary(d3ptmark-le FALSE)
endif()

if(PATHTRACED)
	add_engine_binary(d3ptmark-pt TRUE)
	set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT d3ptmark-pt)
endif()

# game libraries, shared by both binaries
add_library(base SHARED ${src_game})
set_target_properties(base PROPERTIES PREFIX "" LINK_FLAGS "${ldflags}")
target_compile_definitions(base PRIVATE GAME_DLL)
target_include_directories(base PRIVATE "${CMAKE_SOURCE_DIR}/game")
target_link_libraries(base idlib)

add_library(d3xp SHARED ${src_d3xp})
set_target_properties(d3xp PROPERTIES PREFIX "" LINK_FLAGS "${ldflags}")
target_compile_definitions(d3xp PRIVATE GAME_DLL _D3XP CTF)
target_include_directories(d3xp PRIVATE "${CMAKE_SOURCE_DIR}/d3xp")
target_link_libraries(d3xp idlib)

# ---------------------------------------------------------------------------
# path tracer regression tests
# ---------------------------------------------------------------------------

if(TESTS)
	enable_testing()

	set(pt_test_defines "")
	if(PATHTRACED)
		set(pt_test_defines PT_PATHTRACED HAVE_PATHTRACER PT_PHYSICAL)
		if(PT_HAVE_CUDA)
			list(APPEND pt_test_defines HAVE_CUDA)
		endif()
		if(PT_HAVE_OPENCL)
			list(APPEND pt_test_defines HAVE_OPENCL)
		endif()
	endif()

	function(pt_cpu_test name)
		add_executable(${name} renderer_pt/tests/${name}.cpp)
		target_compile_definitions(${name} PRIVATE ${pt_test_defines})
		target_link_libraries(${name} idlib)
		add_test(NAME ${name} COMMAND ${name})
	endfunction()

	# CUDA tests skip (exit code 77) when no CUDA device/driver is available
	function(pt_cuda_test name)
		add_executable(${name} renderer_pt/tests/${name}.cu ${ARGN})
		target_compile_definitions(${name} PRIVATE ${pt_test_defines})
		add_test(NAME ${name} COMMAND ${name})
		set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
	endfunction()

	# OpenCL tests skip (exit code 77) when no OpenCL runtime/GPU is available
	function(pt_opencl_test name)
		add_executable(${name} renderer_pt/tests/${name}.cpp ${src_pt_opencl_launch})
		target_compile_definitions(${name} PRIVATE ${pt_test_defines} CL_TARGET_OPENCL_VERSION=120)
		target_include_directories(${name} PRIVATE ${PT_OPENCL_INCLUDE_DIR})
		# pt_opencl_kernels.cpp uses idStr/idList
		target_link_libraries(${name} idlib ${CMAKE_DL_LIBS})
		add_test(NAME ${name} COMMAND ${name})
		set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
	endfunction()

	pt_cpu_test(pt_capture_report_test)

	# provider matching and the Linux sysfs provider against a fake /sys tree
	add_executable(pt_gpu_telemetry_test renderer_pt/tests/pt_gpu_telemetry_test.cpp renderer_pt/pt_gpu_telemetry.cpp)
	target_link_libraries(pt_gpu_telemetry_test idlib ${CMAKE_DL_LIBS})
	add_test(NAME pt_gpu_telemetry_test COMMAND pt_gpu_telemetry_test)
	pt_cpu_test(pt_benchmark_test)
	pt_cpu_test(pt_material_cache_test)
	pt_cpu_test(pt_quality_preset_test)

	if(PATHTRACED)
		pt_cpu_test(pt_material_editor_test)
	endif()
	if(PT_HAVE_CUDA)
		pt_cuda_test(pt_cuda_lighting_test)
		pt_cuda_test(pt_cuda_denoising_test renderer_pt/cuda/pt_cuda_kernels.cu)
		pt_cuda_test(pt_cuda_postprocess_test)
		pt_cuda_test(pt_cuda_physical_test)
	endif()
	if(PT_HAVE_OPENCL)
		pt_opencl_test(pt_opencl_lighting_test)
		pt_opencl_test(pt_opencl_denoising_test)
		pt_opencl_test(pt_opencl_postprocess_test)
		pt_opencl_test(pt_opencl_physical_test)
		pt_opencl_test(pt_opencl_lbvh_test)
	endif()
endif()
