// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2004-2016 Christoph Oelckers
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
//--------------------------------------------------------------------------
//
/*
** gl_scene.cpp
** manages the rendering of the player's view
**
*/

#include "gi.h"
#include "a_dynlight.h"
#include "m_png.h"
#include "doomstat.h"
#include "r_data/r_interpolate.h"
#include "r_utility.h"
#include "d_player.h"
#include "i_time.h"
#include "swrenderer/r_swscene.h"
#include "swrenderer/r_renderer.h"
#include "hw_dynlightdata.h"
#include "hw_clock.h"
#include "flatvertices.h"
#include "v_palette.h"
#include "d_main.h"
#include "g_cvars.h"
#include "v_draw.h"
#include "hw_vrmodes.h"
#include "dgpu_openxr_session.h"

EXTERN_CVAR(Int, vr_mode)
EXTERN_CVAR(Float, vr_hunits_per_meter)
EXTERN_CVAR(Bool, vr_swap_eyes)

namespace
{
	// set once per real frame (mainview && toscreen) by BeginOpenXRFrame(), read by
	// ApplyOpenXREye() for every eye in that same frame's loop.
	bool openxrFrameActive = false;

	// standard quaternion -> yaw/pitch/roll decomposition, entirely in openxr's own space
	// (right-handed, x-right, y-up, -z-forward). checked by hand against the identity and
	// each pure single-axis rotation, so the *magnitudes* here are solid. what's NOT checked:
	// which of these signs is the one doom actually wants once mapped onto Angles below -
	// there's no headset on this machine to look through and confirm which way is "right".
	// if look direction ends up backwards, mirrored, or upside-down, flip the sign at the
	// point it gets assigned into mainvp.Angles rather than anywhere in here.
	void QuatToOpenXRAngles(const DGpuPose &pose, double &yawDeg, double &pitchDeg, double &rollDeg)
	{
		double x = pose.qx, y = pose.qy, z = pose.qz, w = pose.qw;

		double fx = -2.0 * (x * z + w * y);
		double fy = 2.0 * (w * x - y * z);
		double fz = 2.0 * (x * x + y * y) - 1.0;
		if (fy > 1.0) fy = 1.0;
		if (fy < -1.0) fy = -1.0;

		double yaw = atan2(fx, -fz);
		double pitch = asin(fy);
		double roll = atan2(2.0 * (w * z + x * y), 1.0 - 2.0 * (x * x + z * z));

		const double rad2deg = 180.0 / M_PI;
		yawDeg = yaw * rad2deg;
		pitchDeg = pitch * rad2deg;
		rollDeg = roll * rad2deg;
	}

	// true once BeginOpenXRFrame() has captured the player's pre-vr facing direction as the
	// "zero" for head yaw. reset whenever tracking (re)starts so re-entering vr_mode 15 later
	// recalibrates to wherever the player happens to be facing at that moment, instead of
	// snapping the view to whatever direction was captured the first time.
	bool openxrCalibrated = false;
	double openxrCalibrationYawDeg = 0.0;

	void BeginOpenXRFrame(FRenderViewpoint &mainvp)
	{
		openxrFrameActive = false;
		if (vr_mode != VR_OPENXR)
		{
			openxrCalibrated = false;
			return;
		}

		auto &xr = DGpuOpenXRSession::Get();
		if (!xr.IsActive())
		{
			openxrCalibrated = false;
			return; // no runtime installed, or the lifecycle in VulkanRenderDevice hasn't caught up yet
		}

		DGpuPose headPose;
		if (!xr.BeginFrame(headPose))
			return; // just a skipped frame (not focused yet, etc) - leave calibration alone

		double yawDeg, pitchDeg, rollDeg;
		QuatToOpenXRAngles(headPose, yawDeg, pitchDeg, rollDeg);

		if (!openxrCalibrated)
		{
			openxrCalibrationYawDeg = mainvp.Angles.Yaw.Degrees() + yawDeg;
			openxrCalibrated = true;
		}

		// overriding Angles (not just HWAngles) on purpose - CreateScene's BSP clip range is
		// computed from Angles.Yaw directly, not HWAngles.Yaw, so a HWAngles-only override
		// would clip the scene for the old facing direction while rendering the new one.
		// mainvp here is r_viewpoint for the real frame, not the live player actor, so this
		// doesn't touch aim/movement/hitscan - just this frame's render.
		mainvp.Angles.Yaw = DAngle::fromDeg(openxrCalibrationYawDeg - yawDeg);
		mainvp.Angles.Pitch = DAngle::fromDeg(-pitchDeg);
		mainvp.Angles.Roll = DAngle::fromDeg(rollDeg);

		// HWAngles.Yaw gets rederived from Angles.Yaw automatically (SetViewAngle, called once
		// per eye inside SetupView) but Pitch/Roll aren't - R_SetupFrame sets those once, early,
		// straight off Angles.Pitch/Roll (with a pixelstretch correction on pitch that this
		// skips - a near-invisible approximation given everything else here is a first pass
		// too). set them directly so they don't stay stuck on the pre-override values.
		mainvp.HWAngles.Pitch = FAngle::fromDeg((float)-pitchDeg);
		mainvp.HWAngles.Roll = FAngle::fromDeg((float)rollDeg);

		openxrFrameActive = true;
	}

	// feeds this eye's real tracked position/fov into vrmi_openxr for GetProjection/GetViewShift
	// to pick up. hwYawDeg is read fresh each eye (SetupView recomputes it from Angles.Yaw,
	// which BeginOpenXRFrame already overrode above), so the position shift and the camera's
	// actual look direction stay consistent with each other.
	void ApplyOpenXREye(VREyeInfo &eye, int eyeIndex, double hwYawDeg)
	{
		if (!openxrFrameActive)
		{
			eye.mHasPoseOverride = false;
			return;
		}

		const DGpuEyeFrame &ef = DGpuOpenXRSession::Get().GetEye(eyeIndex);

		// openxr: x = right, y = up, z = back (meters, absolute in the runtime's local space -
		// this is real per-eye ipd *and* any physical lean/head movement, not just a fixed
		// ipd constant). forward/back lean isn't applied to the world position yet - would
		// need its own basis vector derived from hwYawDeg and there's no headset here to
		// verify the sign against, so left at 0 rather than guess.
		double rightMeters = vr_swap_eyes ? -ef.pose.px : ef.pose.px;
		double upMeters = ef.pose.py;

		double rightUnits = rightMeters * vr_hunits_per_meter;
		double upUnits = upMeters * vr_hunits_per_meter;

		// same right-vector convention GetViewShift's fallback formula already uses
		// (dx = -cos(yaw), dy = sin(yaw) for a positive rightward shift), just reused here
		// instead of re-derived so the sign is guaranteed consistent with every other mode.
		double yaw = hwYawDeg * (M_PI / 180.0);
		eye.mOverrideShift.X = -cos(yaw) * rightUnits;
		eye.mOverrideShift.Y = sin(yaw) * rightUnits;
		eye.mOverrideShift.Z = upUnits;

		eye.mOverrideFovLeft = ef.fovLeft;
		eye.mOverrideFovRight = ef.fovRight;
		eye.mOverrideFovUp = ef.fovUp;
		eye.mOverrideFovDown = ef.fovDown;
		eye.mHasPoseOverride = true;
	}
}

#include "hw_lightbuffer.h"
#include "hw_bonebuffer.h"
#include "hw_cvars.h"
#include "hwrenderer/data/hw_viewpointbuffer.h"
#include "hwrenderer/scene/hw_fakeflat.h"
#include "hwrenderer/scene/hw_clipper.h"
#include "hwrenderer/scene/hw_portal.h"
#include "hw_vrmodes.h"

EXTERN_CVAR(Bool, cl_capfps)
extern bool NoInterpolateView;

static SWSceneDrawer *swdrawer;

void CleanSWDrawer()
{
	if (swdrawer) delete swdrawer;
	swdrawer = nullptr;
}

#include "g_levellocals.h"
#include "a_dynlight.h"


void CollectLights(FLevelLocals* Level)
{
	IShadowMap* sm = &screen->mShadowMap;
	int lightindex = 0;

	// Todo: this should go through the blockmap in a spiral pattern around the player so that closer lights are preferred.
	for (auto light = Level->lights; light; light = light->next)
	{
		IShadowMap::LightsProcessed++;
		if (light->shadowmapped && light->IsActive() && lightindex < 1024)
		{
			IShadowMap::LightsShadowmapped++;

			light->mShadowmapIndex = lightindex;
			sm->SetLight(lightindex, (float)light->X(), (float)light->Y(), (float)light->Z(), light->GetRadius());
			lightindex++;
		}
		else
		{
			light->mShadowmapIndex = 1024;
		}

	}

	for (; lightindex < 1024; lightindex++)
	{
		sm->SetLight(lightindex, 0, 0, 0, 0);
	}
}


//-----------------------------------------------------------------------------
//
// Renders one viewpoint in a scene
//
//-----------------------------------------------------------------------------

sector_t* RenderViewpoint(FRenderViewpoint& mainvp, AActor* camera, IntRect* bounds, float fov, float ratio, float fovratio, bool mainview, bool toscreen)
{
	auto& RenderState = *screen->RenderState();

	R_SetupFrame(mainvp, r_viewwindow, camera);

	if (mainview && toscreen && !(camera->Level->flags3 & LEVEL3_NOSHADOWMAP) && camera->Level->HasDynamicLights && gl_light_shadowmap && screen->allowSSBO() && (screen->hwcaps & RFL_SHADER_STORAGE_BUFFER))
	{
		screen->SetAABBTree(camera->Level->aabbTree);
		screen->mShadowMap.SetCollectLights([=] {
			CollectLights(camera->Level);
		});
		screen->UpdateShadowMap();
	}
	else
	{
		// null all references to the level if we do not need a shadowmap. This will shortcut all internal calculations without further checks.
		screen->SetAABBTree(nullptr);
		screen->mShadowMap.SetCollectLights(nullptr);
	}

	screen->SetLevelMesh(camera->Level->levelMesh);

	// Update the attenuation flag of all light defaults for each viewpoint.
	// This function will only do something if the setting differs.
	FLightDefaults::SetAttenuationForLevel(!!(camera->Level->flags3 & LEVEL3_ATTENUATE));

	// Render (potentially) multiple views for stereo 3d
	// Fixme. The view offsetting should be done with a static table and not require setup of the entire render state for the mode.
	auto vrmode = VRMode::GetVRMode(mainview && toscreen);
	const int eyeCount = vrmode->mEyeCount;
	if (mainview && toscreen)
		BeginOpenXRFrame(mainvp);
	screen->FirstEye();
	for (int eye_ix = 0; eye_ix < eyeCount; ++eye_ix)
	{
		const auto& eye = vrmode->mEyes[eye_ix];
		screen->SetViewportRects(bounds);

		if (mainview) // Bind the scene frame buffer and turn on draw buffers used by ssao
		{
			bool useSSAO = (gl_ssao != 0);
			screen->SetSceneRenderTarget(useSSAO);
			RenderState.SetPassType(useSSAO ? GBUFFER_PASS : NORMAL_PASS);
			RenderState.EnableDrawBuffers(RenderState.GetPassDrawBufferCount(), true);
		}

		auto di = HWDrawInfo::StartDrawInfo(mainvp.ViewLevel, nullptr, mainvp, nullptr);
		auto& vp = di->Viewpoint;

		di->Set3DViewport(RenderState);
		di->SetViewArea();
		auto cm = di->SetFullbrightFlags(mainview ? vp.camera->player : nullptr);
		float flash = 1.f;

		// Only used by the GLES2 renderer
		RenderState.SetSpecialColormap(cm, flash);

		di->Viewpoint.FieldOfView = DAngle::fromDeg(fov);	// Set the real FOV for the current scene (it's not necessarily the same as the global setting in r_viewpoint)

		if (mainview && toscreen)
			ApplyOpenXREye(vrmi_openxr.mEyes[eye_ix], eye_ix, vp.HWAngles.Yaw.Degrees());

		// Stereo mode specific perspective projection
		float inv_iso_dist = 1.0f;
		bool iso_ortho = (camera->ViewPos != NULL) && (camera->ViewPos->Flags & VPSF_ORTHOGRAPHIC);
		if (iso_ortho && (camera->ViewPos->Offset.Length() > 0)) inv_iso_dist = 1.0/camera->ViewPos->Offset.Length();
		di->VPUniforms.mProjectionMatrix = eye.GetProjection(fov, ratio, fovratio * inv_iso_dist, iso_ortho);
		di->ProjectionMatrix2 = eye.GetProjection(fov, ratio, fovratio, false); // Regular ol' perspective projection matrix

		// Stereo mode specific viewpoint adjustment
		vp.Pos += eye.GetViewShift(vp.HWAngles.Yaw.Degrees());
		di->SetupView(RenderState, vp.Pos.X, vp.Pos.Y, vp.Pos.Z, false, false);

		di->ProcessScene(toscreen);

		if (mainview)
		{
			PostProcess.Clock();
			if (toscreen) di->EndDrawScene(mainvp.sector, RenderState); // do not call this for camera textures.

			if (RenderState.GetPassType() == GBUFFER_PASS) // Turn off ssao draw buffers
			{
				RenderState.SetPassType(NORMAL_PASS);
				RenderState.EnableDrawBuffers(1);
			}

			screen->PostProcessScene(false, cm, flash, [&]() { di->DrawEndScene2D(mainvp.sector, RenderState); });
			PostProcess.Unclock();

			// this eye's finished image is about to be overwritten by the next eye (vrmi_openxr
			// renders both full-size into the same target, sequentially - there's no side by
			// side split to crop out of), so pull it into the runtime's swapchain now.
			if (openxrFrameActive)
				DGpuOpenXRSession::Get().SubmitEye(eye_ix, 0, 0, screen->mScreenViewport.width, screen->mScreenViewport.height);
		}
		// Reset colormap so 2D drawing isn't affected
		RenderState.SetSpecialColormap(CM_DEFAULT, 1);

		di->EndDrawInfo();
		if (eyeCount - eye_ix > 1)
			screen->NextEye(eyeCount);
	}

	if (openxrFrameActive)
		DGpuOpenXRSession::Get().EndFrame();

	return mainvp.sector;
}

void DoWriteSavePic(FileWriter* file, ESSType ssformat, uint8_t* scr, int width, int height, sector_t* viewsector, bool upsidedown)
{
	PalEntry palette[256];
	PalEntry modulateColor;
	auto blend = V_CalcBlend(viewsector, &modulateColor);
	int pixelsize = 1;
	// Apply the screen blend, because the renderer does not provide this.
	if (ssformat == SS_RGB)
	{
		int numbytes = width * height * 3;
		pixelsize = 3;
		if (modulateColor != 0xffffffff)
		{
			float r = modulateColor.r / 255.f;
			float g = modulateColor.g / 255.f;
			float b = modulateColor.b / 255.f;
			for (int i = 0; i < numbytes; i += 3)
			{
				scr[i] = uint8_t(scr[i] * r);
				scr[i + 1] = uint8_t(scr[i + 1] * g);
				scr[i + 2] = uint8_t(scr[i + 2] * b);
			}
		}
		float iblendfac = 1.f - blend.W;
		blend.X *= blend.W;
		blend.Y *= blend.W;
		blend.Z *= blend.W;
		for (int i = 0; i < numbytes; i += 3)
		{
			scr[i] = uint8_t(scr[i] * iblendfac + blend.X);
			scr[i + 1] = uint8_t(scr[i + 1] * iblendfac + blend.Y);
			scr[i + 2] = uint8_t(scr[i + 2] * iblendfac + blend.Z);
		}
	}
	else
	{
		// Apply the screen blend to the palette. The colormap related parts get skipped here because these are already part of the image.
		DoBlending(GPalette.BaseColors, palette, 256, uint8_t(blend.X), uint8_t(blend.Y), uint8_t(blend.Z), uint8_t(blend.W * 255));
	}

	int pitch = width * pixelsize;
	if (upsidedown)
	{
		scr += ((height - 1) * width * pixelsize);
		pitch *= -1;
	}

	M_CreatePNG(file, scr, ssformat == SS_PAL ? palette : nullptr, ssformat, width, height, pitch, vid_gamma);
}

//===========================================================================
//
// Render the view to a savegame picture
//
//===========================================================================

void WriteSavePic(player_t* player, FileWriter* file, int width, int height)
{
	if (!V_IsHardwareRenderer())
	{
		SWRenderer->WriteSavePic(player, file, width, height);
	}
	else
	{
		IntRect bounds;
		bounds.left = 0;
		bounds.top = 0;
		bounds.width = width;
		bounds.height = height;
		auto& RenderState = *screen->RenderState();

		// we must be sure the GPU finished reading from the buffer before we fill it with new data.
		screen->WaitForCommands(false);

		// Switch to render buffers dimensioned for the savepic
		screen->SetSaveBuffers(true);
		screen->ImageTransitionScene(true);

		hw_postprocess.SetTonemapMode(level.info ? level.info->tonemap : ETonemapMode::None);
		hw_ClearFakeFlat();
		screen->mVertexData->Reset();
		RenderState.SetVertexBuffer(screen->mVertexData);
		screen->mLights->Clear();
		screen->mBones->Clear();
		screen->mViewpoints->Clear();

		// This shouldn't overwrite the global viewpoint even for a short time.
		FRenderViewpoint savevp;
		sector_t* viewsector = RenderViewpoint(savevp, players[consoleplayer].camera, &bounds, r_viewpoint.FieldOfView.Degrees(), 1.6f, 1.6f, true, false);
		RenderState.EnableStencil(false);
		RenderState.SetNoSoftLightLevel();

		TArray<uint8_t> scr(width * height * 3, true);
		screen->CopyScreenToBuffer(width, height, scr.Data());

		DoWriteSavePic(file, SS_RGB, scr.Data(), width, height, viewsector, screen->FlipSavePic());

		// Switch back the screen render buffers
		screen->SetViewportRects(nullptr);
		screen->SetSaveBuffers(false);
	}
}

//===========================================================================
//
// Renders the main view
//
//===========================================================================

static void CheckTimer(FRenderState &state, uint64_t ShaderStartTime)
{
	// if firstFrame is not yet initialized, initialize it to current time
	// if we're going to overflow a float (after ~4.6 hours, or 24 bits), re-init to regain precision
	if ((state.firstFrame == 0) || (screen->FrameTime - state.firstFrame >= 1 << 24) || ShaderStartTime >= state.firstFrame)
		state.firstFrame = screen->FrameTime - 1;
}


sector_t* RenderView(player_t* player)
{
	auto RenderState = screen->RenderState();
	RenderState->SetVertexBuffer(screen->mVertexData);
	screen->mVertexData->Reset();
	hw_postprocess.SetTonemapMode(level.info ? level.info->tonemap : ETonemapMode::None);

	sector_t* retsec;
	if (!V_IsHardwareRenderer())
	{
		screen->SetActiveRenderTarget();	// only relevant for Vulkan

		if (!swdrawer) swdrawer = new SWSceneDrawer;
		retsec = swdrawer->RenderView(player);
	}
	else
	{
		hw_ClearFakeFlat();

		iter_dlightf = iter_dlight = draw_dlight = draw_dlightf = 0;

		checkBenchActive();

		// reset statistics counters
		ResetProfilingData();

		// Get this before everything else
		if (cl_capfps || r_NoInterpolate) r_viewpoint.TicFrac = 1.;
		else r_viewpoint.TicFrac = I_GetTimeFrac();

		screen->mLights->Clear();
		screen->mBones->Clear();
		screen->mViewpoints->Clear();

		// NoInterpolateView should have no bearing on camera textures, but needs to be preserved for the main view below.
		bool saved_niv = NoInterpolateView;
		NoInterpolateView = false;

		// Shader start time does not need to be handled per level. Just use the one from the camera to render from.
		if (player->camera)
			CheckTimer(*RenderState, player->camera->Level->ShaderStartTime);

		// Draw all canvases that changed
		for (FCanvas* canvas : AllCanvases)
		{
			if (canvas->Tex && canvas->Tex->CheckNeedsUpdate())
			{
				screen->RenderTextureView(canvas->Tex, [=](IntRect& bounds)
					{
						screen->SetViewportRects(&bounds);
						Draw2D(&canvas->Drawer, *screen->RenderState(), 0, 0, canvas->Tex->GetWidth(), canvas->Tex->GetHeight());
						canvas->Drawer.Clear();
					});
				canvas->Tex->SetUpdated(true);
			}
		}

		// prepare all camera textures that have been used in the last frame.
		// This must be done for all levels, not just the primary one!
		for (auto Level : AllLevels())
		{
			Level->canvasTextureInfo.UpdateAll([&](AActor* camera, FCanvasTexture* camtex, double fov)
				{
					screen->RenderTextureView(camtex, [=](IntRect& bounds)
						{
							FRenderViewpoint texvp;
							float ratio = camtex->aspectRatio / Level->info->pixelstretch;
							RenderViewpoint(texvp, camera, &bounds, fov, ratio, ratio, false, false);
						});
				});
		}
		NoInterpolateView = saved_niv;

		// now render the main view
		float fovratio;
		float ratio = r_viewwindow.WidescreenRatio;
		if (r_viewwindow.WidescreenRatio >= 1.3f)
		{
			fovratio = 1.333333f;
		}
		else
		{
			fovratio = ratio;
		}

		screen->ImageTransitionScene(true); // Only relevant for Vulkan.

		retsec = RenderViewpoint(r_viewpoint, player->camera, NULL, r_viewpoint.FieldOfView.Degrees(), ratio, fovratio, true, true);
	}
	All.Unclock();
	return retsec;
}

