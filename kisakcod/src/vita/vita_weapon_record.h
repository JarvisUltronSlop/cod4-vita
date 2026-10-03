#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace kisak::vita {

inline constexpr uint32_t kVitaWeaponMagic = 0x50544956; // "VITP"
inline constexpr uint32_t kVitaWeaponVersionSource = 5;
inline constexpr uint32_t kVitaWeaponSourceLayoutVersion = 1;
inline constexpr uint32_t kVitaWeaponNameBytes = 64;
inline constexpr uint32_t kVitaWeaponAnimationCount = 33;
inline constexpr uint32_t kVitaWeaponGunModelCount = 16;
inline constexpr uint32_t kVitaWeaponWorldModelCount = 16;
inline constexpr uint32_t kVitaWeaponHideTagCount = 8;
inline constexpr uint32_t kVitaWeaponNotetrackCount = 16;
inline constexpr uint32_t kVitaWeaponBounceSoundCount = 23;
inline constexpr uint32_t kVitaWeaponBounceCurveCount = 29;
inline constexpr uint32_t kVitaWeaponLocationDamageCount = 19;

// These X-macro lists are the scalar wire contract. The second argument is
// the genuine WeaponDef member populated by the Windows owner/exporter and
// restored by the Vita typed constructor. Keeping that mapping in one place
// prevents an offset table from quietly becoming an alternative WeaponDef.
#define KISAK_VITA_WEAPON_INT_FIELDS(X)                                    \
    X(PlayerAnimType, playerAnimType)                                      \
    X(WeaponType, weapType)                                                \
    X(WeaponClass, weapClass)                                              \
    X(PenetrateType, penetrateType)                                        \
    X(ImpactType, impactType)                                              \
    X(InventoryType, inventoryType)                                        \
    X(FireType, fireType)                                                  \
    X(OffhandClass, offhandClass)                                          \
    X(Stance, stance)                                                      \
    X(ReticleCenterSize, iReticleCenterSize)                               \
    X(ReticleSideSize, iReticleSideSize)                                   \
    X(ReticleMinOffset, iReticleMinOfs)                                    \
    X(ActiveReticleType, activeReticleType)                                \
    X(HudIconRatio, hudIconRatio)                                          \
    X(AmmoCounterIconRatio, ammoCounterIconRatio)                          \
    X(AmmoCounterClip, ammoCounterClip)                                    \
    X(StartAmmo, iStartAmmo)                                               \
    X(MaxAmmo, iMaxAmmo)                                                   \
    X(ClipSize, iClipSize)                                                 \
    X(ShotCount, shotCount)                                                \
    X(SharedAmmoCap, iSharedAmmoCap)                                       \
    X(Damage, damage)                                                      \
    X(PlayerDamage, playerDamage)                                          \
    X(MeleeDamage, iMeleeDamage)                                           \
    X(DamageType, iDamageType)                                             \
    X(FireDelay, iFireDelay)                                               \
    X(MeleeDelay, iMeleeDelay)                                             \
    X(MeleeChargeDelay, meleeChargeDelay)                                  \
    X(DetonateDelay, iDetonateDelay)                                       \
    X(FireTime, iFireTime)                                                 \
    X(RechamberTime, iRechamberTime)                                       \
    X(RechamberBoltTime, iRechamberBoltTime)                               \
    X(HoldFireTime, iHoldFireTime)                                         \
    X(DetonateTime, iDetonateTime)                                         \
    X(MeleeTime, iMeleeTime)                                               \
    X(MeleeChargeTime, meleeChargeTime)                                    \
    X(ReloadTime, iReloadTime)                                             \
    X(ReloadShowRocketTime, reloadShowRocketTime)                           \
    X(ReloadEmptyTime, iReloadEmptyTime)                                   \
    X(ReloadAddTime, iReloadAddTime)                                       \
    X(ReloadStartTime, iReloadStartTime)                                   \
    X(ReloadStartAddTime, iReloadStartAddTime)                             \
    X(ReloadEndTime, iReloadEndTime)                                       \
    X(DropTime, iDropTime)                                                 \
    X(RaiseTime, iRaiseTime)                                               \
    X(AltDropTime, iAltDropTime)                                           \
    X(AltRaiseTime, iAltRaiseTime)                                         \
    X(QuickDropTime, quickDropTime)                                        \
    X(QuickRaiseTime, quickRaiseTime)                                      \
    X(FirstRaiseTime, iFirstRaiseTime)                                     \
    X(EmptyRaiseTime, iEmptyRaiseTime)                                     \
    X(EmptyDropTime, iEmptyDropTime)                                       \
    X(SprintInTime, sprintInTime)                                          \
    X(SprintLoopTime, sprintLoopTime)                                      \
    X(SprintOutTime, sprintOutTime)                                        \
    X(NightVisionWearTime, nightVisionWearTime)                            \
    X(NightVisionWearFadeOutEnd, nightVisionWearTimeFadeOutEnd)            \
    X(NightVisionWearPowerUp, nightVisionWearTimePowerUp)                  \
    X(NightVisionRemoveTime, nightVisionRemoveTime)                        \
    X(NightVisionRemovePowerDown, nightVisionRemoveTimePowerDown)          \
    X(NightVisionRemoveFadeInStart, nightVisionRemoveTimeFadeInStart)      \
    X(FuseTime, fuseTime)                                                   \
    X(AiFuseTime, aiFuseTime)                                              \
    X(RequireLockonToFire, requireLockonToFire)                            \
    X(NoAdsWhenMagEmpty, noAdsWhenMagEmpty)                                \
    X(AvoidDropCleanup, avoidDropCleanup)                                  \
    X(CrosshairColorChange, crosshairColorChange)                          \
    X(RifleBullet, bRifleBullet)                                           \
    X(ArmorPiercing, armorPiercing)                                        \
    X(BoltAction, bBoltAction)                                             \
    X(AimDownSight, aimDownSight)                                          \
    X(RechamberWhileAds, bRechamberWhileAds)                               \
    X(CookOffHold, bCookOffHold)                                           \
    X(ClipOnly, bClipOnly)                                                 \
    X(AdsFireOnly, adsFireOnly)                                            \
    X(CancelAutoHolsterWhenEmpty, cancelAutoHolsterWhenEmpty)              \
    X(SuppressAmmoReserveDisplay, suppressAmmoReserveDisplay)              \
    X(Enhanced, enhanced)                                                  \
    X(LaserSightDuringNightvision, laserSightDuringNightvision)            \
    X(KillIconRatio, killIconRatio)                                        \
    X(FlipKillIcon, flipKillIcon)                                          \
    X(DpadIconRatio, dpadIconRatio)                                        \
    X(NoPartialReload, bNoPartialReload)                                   \
    X(SegmentedReload, bSegmentedReload)                                   \
    X(ReloadAmmoAdd, iReloadAmmoAdd)                                       \
    X(ReloadStartAdd, iReloadStartAdd)                                     \
    X(DropAmmoMin, iDropAmmoMin)                                           \
    X(DropAmmoMax, iDropAmmoMax)                                           \
    X(BlocksProne, blocksProne)                                            \
    X(Silenced, silenced)                                                  \
    X(ExplosionRadius, iExplosionRadius)                                   \
    X(ExplosionRadiusMin, iExplosionRadiusMin)                             \
    X(ExplosionInnerDamage, iExplosionInnerDamage)                         \
    X(ExplosionOuterDamage, iExplosionOuterDamage)                         \
    X(ProjectileSpeed, iProjectileSpeed)                                   \
    X(ProjectileSpeedUp, iProjectileSpeedUp)                               \
    X(ProjectileSpeedForward, iProjectileSpeedForward)                     \
    X(ProjectileActivateDistance, iProjectileActivateDist)                 \
    X(ProjectileExplosion, projExplosion)                                  \
    X(ProjectileExplosionForceNormalUp, projExplosionEffectForceNormalUp)  \
    X(ProjectileImpactExplode, bProjImpactExplode)                         \
    X(Stickiness, stickiness)                                              \
    X(HasDetonator, hasDetonator)                                          \
    X(TimedDetonation, timedDetonation)                                    \
    X(Rotate, rotate)                                                      \
    X(HoldButtonToThrow, holdButtonToThrow)                                \
    X(FreezeMovementWhenFiring, freezeMovementWhenFiring)                  \
    X(GuidedMissileType, guidedMissileType)                                \
    X(ProjectileIgnitionDelay, projIgnitionDelay)                          \
    X(AdsGunKickReducedBullets, adsGunKickReducedKickBullets)               \
    X(HipGunKickReducedBullets, hipGunKickReducedKickBullets)               \
    X(PositionReloadTransitionTime, iPositionReloadTransTime)              \
    X(UseHintStringIndex, iUseHintStringIndex)                              \
    X(DropHintStringIndex, dropHintStringIndex)                             \
    X(MinDamage, minDamage)                                                \
    X(MinPlayerDamage, minPlayerDamage)                                    \
    X(DestabilizeDistance, destabilizeDistance)

#define KISAK_VITA_WEAPON_FLOAT_FIELDS(X)                                  \
    X(PosMoveRate, fPosMoveRate)                                           \
    X(PosProneMoveRate, fPosProneMoveRate)                                 \
    X(StandMoveMinSpeed, fStandMoveMinSpeed)                               \
    X(DuckedMoveMinSpeed, fDuckedMoveMinSpeed)                             \
    X(ProneMoveMinSpeed, fProneMoveMinSpeed)                               \
    X(PosRotRate, fPosRotRate)                                             \
    X(PosProneRotRate, fPosProneRotRate)                                   \
    X(StandRotMinSpeed, fStandRotMinSpeed)                                 \
    X(DuckedRotMinSpeed, fDuckedRotMinSpeed)                               \
    X(ProneRotMinSpeed, fProneRotMinSpeed)                                 \
    X(AutoAimRange, autoAimRange)                                          \
    X(AimAssistRange, aimAssistRange)                                      \
    X(AimAssistRangeAds, aimAssistRangeAds)                                \
    X(AimPadding, aimPadding)                                              \
    X(EnemyCrosshairRange, enemyCrosshairRange)                            \
    X(MoveSpeedScale, moveSpeedScale)                                      \
    X(AdsMoveSpeedScale, adsMoveSpeedScale)                                \
    X(SprintDurationScale, sprintDurationScale)                            \
    X(AdsZoomFov, fAdsZoomFov)                                             \
    X(AdsZoomInFraction, fAdsZoomInFrac)                                   \
    X(AdsZoomOutFraction, fAdsZoomOutFrac)                                 \
    X(OverlayWidth, overlayWidth)                                          \
    X(OverlayHeight, overlayHeight)                                        \
    X(AdsBobFactor, fAdsBobFactor)                                         \
    X(AdsViewBobMultiplier, fAdsViewBobMult)                               \
    X(HipSpreadStandMin, fHipSpreadStandMin)                               \
    X(HipSpreadDuckedMin, fHipSpreadDuckedMin)                             \
    X(HipSpreadProneMin, fHipSpreadProneMin)                               \
    X(HipSpreadStandMax, hipSpreadStandMax)                                \
    X(HipSpreadDuckedMax, hipSpreadDuckedMax)                              \
    X(HipSpreadProneMax, hipSpreadProneMax)                                \
    X(HipSpreadDecayRate, fHipSpreadDecayRate)                             \
    X(HipSpreadFireAdd, fHipSpreadFireAdd)                                 \
    X(HipSpreadTurnAdd, fHipSpreadTurnAdd)                                 \
    X(HipSpreadMoveAdd, fHipSpreadMoveAdd)                                 \
    X(HipSpreadDuckedDecay, fHipSpreadDuckedDecay)                         \
    X(HipSpreadProneDecay, fHipSpreadProneDecay)                           \
    X(HipReticleSidePosition, fHipReticleSidePos)                          \
    X(AdsIdleAmount, fAdsIdleAmount)                                       \
    X(HipIdleAmount, fHipIdleAmount)                                       \
    X(AdsIdleSpeed, adsIdleSpeed)                                          \
    X(HipIdleSpeed, hipIdleSpeed)                                          \
    X(IdleCrouchFactor, fIdleCrouchFactor)                                 \
    X(IdleProneFactor, fIdleProneFactor)                                   \
    X(GunMaxPitch, fGunMaxPitch)                                           \
    X(GunMaxYaw, fGunMaxYaw)                                               \
    X(SwayMaxAngle, swayMaxAngle)                                          \
    X(SwayLerpSpeed, swayLerpSpeed)                                        \
    X(SwayPitchScale, swayPitchScale)                                      \
    X(SwayYawScale, swayYawScale)                                          \
    X(SwayHorizontalScale, swayHorizScale)                                 \
    X(SwayVerticalScale, swayVertScale)                                    \
    X(SwayShellShockScale, swayShellShockScale)                            \
    X(AdsSwayMaxAngle, adsSwayMaxAngle)                                    \
    X(AdsSwayLerpSpeed, adsSwayLerpSpeed)                                  \
    X(AdsSwayPitchScale, adsSwayPitchScale)                                \
    X(AdsSwayYawScale, adsSwayYawScale)                                    \
    X(AdsSwayHorizontalScale, adsSwayHorizScale)                           \
    X(AdsSwayVerticalScale, adsSwayVertScale)                              \
    X(AdsViewErrorMin, adsViewErrorMin)                                    \
    X(AdsViewErrorMax, adsViewErrorMax)                                    \
    X(DamageConeAngle, damageConeAngle)                                    \
    X(ProjectileLifetime, projLifetime)                                    \
    X(TimeToAccelerate, timeToAccelerate)                                  \
    X(ProjectileCurvature, projectileCurvature)                            \
    X(LowAmmoWarningThreshold, lowAmmoWarningThreshold)                    \
    X(MaxSteeringAcceleration, maxSteeringAccel)                           \
    X(AdsAimPitch, fAdsAimPitch)                                           \
    X(AdsCrosshairInFraction, fAdsCrosshairInFrac)                         \
    X(AdsCrosshairOutFraction, fAdsCrosshairOutFrac)                       \
    X(AdsGunKickReducedPercent, adsGunKickReducedKickPercent)              \
    X(AdsGunKickPitchMin, fAdsGunKickPitchMin)                             \
    X(AdsGunKickPitchMax, fAdsGunKickPitchMax)                             \
    X(AdsGunKickYawMin, fAdsGunKickYawMin)                                 \
    X(AdsGunKickYawMax, fAdsGunKickYawMax)                                 \
    X(AdsGunKickAccel, fAdsGunKickAccel)                                   \
    X(AdsGunKickSpeedMax, fAdsGunKickSpeedMax)                             \
    X(AdsGunKickSpeedDecay, fAdsGunKickSpeedDecay)                         \
    X(AdsGunKickStaticDecay, fAdsGunKickStaticDecay)                       \
    X(AdsViewKickPitchMin, fAdsViewKickPitchMin)                           \
    X(AdsViewKickPitchMax, fAdsViewKickPitchMax)                           \
    X(AdsViewKickYawMin, fAdsViewKickYawMin)                               \
    X(AdsViewKickYawMax, fAdsViewKickYawMax)                               \
    X(AdsViewKickCenterSpeed, fAdsViewKickCenterSpeed)                     \
    X(AdsViewScatterMin, fAdsViewScatterMin)                               \
    X(AdsViewScatterMax, fAdsViewScatterMax)                               \
    X(AdsSpread, fAdsSpread)                                               \
    X(HipGunKickReducedPercent, hipGunKickReducedKickPercent)              \
    X(HipGunKickPitchMin, fHipGunKickPitchMin)                             \
    X(HipGunKickPitchMax, fHipGunKickPitchMax)                             \
    X(HipGunKickYawMin, fHipGunKickYawMin)                                 \
    X(HipGunKickYawMax, fHipGunKickYawMax)                                 \
    X(HipGunKickAccel, fHipGunKickAccel)                                   \
    X(HipGunKickSpeedMax, fHipGunKickSpeedMax)                             \
    X(HipGunKickSpeedDecay, fHipGunKickSpeedDecay)                         \
    X(HipGunKickStaticDecay, fHipGunKickStaticDecay)                       \
    X(HipViewKickPitchMin, fHipViewKickPitchMin)                           \
    X(HipViewKickPitchMax, fHipViewKickPitchMax)                           \
    X(HipViewKickYawMin, fHipViewKickYawMin)                               \
    X(HipViewKickYawMax, fHipViewKickYawMax)                               \
    X(HipViewKickCenterSpeed, fHipViewKickCenterSpeed)                     \
    X(HipViewScatterMin, fHipViewScatterMin)                               \
    X(HipViewScatterMax, fHipViewScatterMax)                               \
    X(FightDistance, fightDist)                                            \
    X(MaxDistance, maxDist)                                                \
    X(LeftArc, leftArc)                                                    \
    X(RightArc, rightArc)                                                  \
    X(TopArc, topArc)                                                      \
    X(BottomArc, bottomArc)                                                \
    X(Accuracy, accuracy)                                                  \
    X(AiSpread, aiSpread)                                                  \
    X(PlayerSpread, playerSpread)                                          \
    X(MinTurnSpeedPitch, minTurnSpeed[0])                                  \
    X(MinTurnSpeedYaw, minTurnSpeed[1])                                    \
    X(MaxTurnSpeedPitch, maxTurnSpeed[0])                                  \
    X(MaxTurnSpeedYaw, maxTurnSpeed[1])                                    \
    X(PitchConvergenceTime, pitchConvergenceTime)                          \
    X(YawConvergenceTime, yawConvergenceTime)                              \
    X(SuppressTime, suppressTime)                                          \
    X(MaxRange, maxRange)                                                  \
    X(AnimationHorizontalRotateIncrement, fAnimHorRotateInc)               \
    X(PlayerPositionDistance, fPlayerPositionDist)                         \
    X(HorizontalViewJitter, horizViewJitter)                               \
    X(VerticalViewJitter, vertViewJitter)                                  \
    X(PositionAnimationReciprocalIn, fOOPosAnimLength[0])                  \
    X(PositionAnimationReciprocalOut, fOOPosAnimLength[1])                 \
    X(MaxDamageRange, fMaxDamageRange)                                     \
    X(MinDamageRange, fMinDamageRange)                                     \
    X(DestabilizationRateTime, destabilizationRateTime)                    \
    X(DestabilizationCurvatureMax, destabilizationCurvatureMax)            \
    X(AdsDofStart, adsDofStart)                                            \
    X(AdsDofEnd, adsDofEnd)

#define KISAK_VITA_WEAPON_STRING_FIELDS(X)                                 \
    X(DisplayName, szDisplayName)                                          \
    X(OverlayName, szOverlayName)                                          \
    X(ModeName, szModeName)                                                \
    X(AmmoName, szAmmoName)                                                \
    X(ClipName, szClipName)                                                \
    X(SharedAmmoCapName, szSharedAmmoCapName)                              \
    X(AltWeaponName, szAltWeaponName)                                      \
    X(UseHintString, szUseHintString)                                      \
    X(DropHintString, dropHintString)                                      \
    X(Script, szScript)                                                    \
    X(FireRumble, fireRumble)                                              \
    X(MeleeImpactRumble, meleeImpactRumble)

#define KISAK_VITA_WEAPON_MATERIAL_FIELDS(X)                               \
    X(ReticleCenter, reticleCenter)                                        \
    X(ReticleSide, reticleSide)                                            \
    X(HudIcon, hudIcon)                                                    \
    X(AmmoCounterIcon, ammoCounterIcon)                                    \
    X(Overlay, overlayMaterial)                                            \
    X(OverlayLowResolution, overlayMaterialLowRes)                         \
    X(KillIcon, killIcon)                                                  \
    X(DpadIcon, dpadIcon)

#define KISAK_VITA_WEAPON_EFFECT_FIELDS(X)                                 \
    X(ViewFlash, viewFlashEffect)                                          \
    X(WorldFlash, worldFlashEffect)                                        \
    X(ViewShellEject, viewShellEjectEffect)                                \
    X(WorldShellEject, worldShellEjectEffect)                              \
    X(ViewLastShotEject, viewLastShotEjectEffect)                          \
    X(WorldLastShotEject, worldLastShotEjectEffect)                        \
    X(ProjectileExplosion, projExplosionEffect)                            \
    X(ProjectileDud, projDudEffect)                                        \
    X(ProjectileTrail, projTrailEffect)                                    \
    X(ProjectileIgnition, projIgnitionEffect)

#define KISAK_VITA_WEAPON_SOUND_FIELDS(X)                                  \
    X(Pickup, pickupSound)                                                  \
    X(PickupPlayer, pickupSoundPlayer)                                     \
    X(AmmoPickup, ammoPickupSound)                                         \
    X(AmmoPickupPlayer, ammoPickupSoundPlayer)                             \
    X(Projectile, projectileSound)                                         \
    X(Pullback, pullbackSound)                                             \
    X(PullbackPlayer, pullbackSoundPlayer)                                 \
    X(Fire, fireSound)                                                     \
    X(FirePlayer, fireSoundPlayer)                                         \
    X(FireLoop, fireLoopSound)                                             \
    X(FireLoopPlayer, fireLoopSoundPlayer)                                 \
    X(FireStop, fireStopSound)                                             \
    X(FireStopPlayer, fireStopSoundPlayer)                                 \
    X(FireLast, fireLastSound)                                             \
    X(FireLastPlayer, fireLastSoundPlayer)                                 \
    X(EmptyFire, emptyFireSound)                                           \
    X(EmptyFirePlayer, emptyFireSoundPlayer)                               \
    X(MeleeSwipe, meleeSwipeSound)                                         \
    X(MeleeSwipePlayer, meleeSwipeSoundPlayer)                             \
    X(MeleeHit, meleeHitSound)                                             \
    X(MeleeMiss, meleeMissSound)                                           \
    X(Rechamber, rechamberSound)                                           \
    X(RechamberPlayer, rechamberSoundPlayer)                               \
    X(Reload, reloadSound)                                                 \
    X(ReloadPlayer, reloadSoundPlayer)                                     \
    X(ReloadEmpty, reloadEmptySound)                                       \
    X(ReloadEmptyPlayer, reloadEmptySoundPlayer)                           \
    X(ReloadStart, reloadStartSound)                                       \
    X(ReloadStartPlayer, reloadStartSoundPlayer)                           \
    X(ReloadEnd, reloadEndSound)                                           \
    X(ReloadEndPlayer, reloadEndSoundPlayer)                               \
    X(Detonate, detonateSound)                                             \
    X(DetonatePlayer, detonateSoundPlayer)                                 \
    X(NightVisionWear, nightVisionWearSound)                               \
    X(NightVisionWearPlayer, nightVisionWearSoundPlayer)                   \
    X(NightVisionRemove, nightVisionRemoveSound)                           \
    X(NightVisionRemovePlayer, nightVisionRemoveSoundPlayer)               \
    X(AltSwitch, altSwitchSound)                                           \
    X(AltSwitchPlayer, altSwitchSoundPlayer)                               \
    X(Raise, raiseSound)                                                   \
    X(RaisePlayer, raiseSoundPlayer)                                       \
    X(FirstRaise, firstRaiseSound)                                         \
    X(FirstRaisePlayer, firstRaiseSoundPlayer)                             \
    X(Putaway, putawaySound)                                               \
    X(PutawayPlayer, putawaySoundPlayer)                                   \
    X(ProjectileExplosion, projExplosionSound)                             \
    X(ProjectileDud, projDudSound)                                         \
    X(ProjectileIgnition, projIgnitionSound)

enum class VitaWeaponIntField : uint32_t {
#define KISAK_VITA_ENUM_FIELD(label, member) label,
    KISAK_VITA_WEAPON_INT_FIELDS(KISAK_VITA_ENUM_FIELD)
#undef KISAK_VITA_ENUM_FIELD
    Count,
};

enum class VitaWeaponFloatField : uint32_t {
#define KISAK_VITA_ENUM_FIELD(label, member) label,
    KISAK_VITA_WEAPON_FLOAT_FIELDS(KISAK_VITA_ENUM_FIELD)
#undef KISAK_VITA_ENUM_FIELD
    Count,
};

enum class VitaWeaponStringField : uint32_t {
#define KISAK_VITA_ENUM_FIELD(label, member) label,
    KISAK_VITA_WEAPON_STRING_FIELDS(KISAK_VITA_ENUM_FIELD)
#undef KISAK_VITA_ENUM_FIELD
    Count,
};

enum class VitaWeaponMaterialField : uint32_t {
#define KISAK_VITA_ENUM_FIELD(label, member) label,
    KISAK_VITA_WEAPON_MATERIAL_FIELDS(KISAK_VITA_ENUM_FIELD)
#undef KISAK_VITA_ENUM_FIELD
    Count,
};

enum class VitaWeaponEffectField : uint32_t {
#define KISAK_VITA_ENUM_FIELD(label, member) label,
    KISAK_VITA_WEAPON_EFFECT_FIELDS(KISAK_VITA_ENUM_FIELD)
#undef KISAK_VITA_ENUM_FIELD
    Count,
};

enum class VitaWeaponSoundField : uint32_t {
#define KISAK_VITA_ENUM_FIELD(label, member) label,
    KISAK_VITA_WEAPON_SOUND_FIELDS(KISAK_VITA_ENUM_FIELD)
#undef KISAK_VITA_ENUM_FIELD
    Count,
};

enum VitaWeaponSourceFlags : uint32_t {
    VitaWeaponSourceScalars = 1u << 0,
    VitaWeaponSourceStrings = 1u << 1,
    VitaWeaponSourceModels = 1u << 2,
    VitaWeaponSourceAnimations = 1u << 3,
    VitaWeaponSourceMaterials = 1u << 4,
    VitaWeaponSourceEffects = 1u << 5,
    VitaWeaponSourceSounds = 1u << 6,
    VitaWeaponSourceTags = 1u << 7,
    VitaWeaponSourceBounce = 1u << 8,
    VitaWeaponSourceDamage = 1u << 9,
};

inline constexpr uint32_t kVitaWeaponSourceRequiredFlags =
    VitaWeaponSourceScalars | VitaWeaponSourceStrings
    | VitaWeaponSourceModels | VitaWeaponSourceAnimations
    | VitaWeaponSourceMaterials | VitaWeaponSourceEffects
    | VitaWeaponSourceSounds | VitaWeaponSourceTags
    | VitaWeaponSourceBounce | VitaWeaponSourceDamage;

// V5 deliberately retains the complete V4 prefix. The scalar oracle and the
// existing native viewmodel can inspect a fresh source record while the
// release runtime consumes only the V5 source tail through named fields.
struct VitaWeaponRecordV5 {
    char name[kVitaWeaponNameBytes];
    uint32_t weaponType;
    uint32_t weaponClass;
    uint32_t fireType;
    uint32_t inventoryType;
    uint32_t stance;
    int32_t startAmmo;
    int32_t maxAmmo;
    int32_t clipSize;
    int32_t damage;
    int32_t playerDamage;
    int32_t fireDelay;
    int32_t fireTime;
    int32_t rechamberTime;
    int32_t reloadTime;
    int32_t reloadEmptyTime;
    int32_t adsTransInTime;
    int32_t adsTransOutTime;
    float moveSpeedScale;
    float adsMoveSpeedScale;
    float sprintDurationScale;
    float adsZoomFov;
    float hipSpreadStandMin;
    float hipSpreadStandMax;
    float adsSwayMaxAngle;
    char viewModelName[kVitaWeaponNameBytes];
    uint64_t hideSurfaceMask;
    uint32_t viewModelSurfaceCount;
    uint32_t hideTagCount;
    uint32_t hideTagResolvedCount;
    char viewAnimationNames[kVitaWeaponAnimationCount][kVitaWeaponNameBytes];
    int32_t freezeMovementWhenFiring;

    uint32_t sourceSchemaVersion;
    uint32_t sourceLayoutVersion;
    uint32_t sourceFlags;
    uint32_t reserved;
    char stringNames[static_cast<uint32_t>(VitaWeaponStringField::Count)]
                    [kVitaWeaponNameBytes];
    char gunModelNames[kVitaWeaponGunModelCount][kVitaWeaponNameBytes];
    char handModelName[kVitaWeaponNameBytes];
    char worldModelNames[kVitaWeaponWorldModelCount][kVitaWeaponNameBytes];
    char worldClipModelName[kVitaWeaponNameBytes];
    char rocketModelName[kVitaWeaponNameBytes];
    char knifeModelName[kVitaWeaponNameBytes];
    char worldKnifeModelName[kVitaWeaponNameBytes];
    char projectileModelName[kVitaWeaponNameBytes];
    char hideTagNames[kVitaWeaponHideTagCount][kVitaWeaponNameBytes];
    char notetrackSoundKeyNames[kVitaWeaponNotetrackCount][kVitaWeaponNameBytes];
    char notetrackSoundValueNames[kVitaWeaponNotetrackCount][kVitaWeaponNameBytes];
    char materialNames[static_cast<uint32_t>(VitaWeaponMaterialField::Count)]
                      [kVitaWeaponNameBytes];
    char effectNames[static_cast<uint32_t>(VitaWeaponEffectField::Count)]
                    [kVitaWeaponNameBytes];
    char soundNames[static_cast<uint32_t>(VitaWeaponSoundField::Count)]
                   [kVitaWeaponNameBytes];
    char bounceSoundNames[kVitaWeaponBounceSoundCount][kVitaWeaponNameBytes];
    char accuracyGraphNames[2][kVitaWeaponNameBytes];
    int32_t integerFields[static_cast<uint32_t>(VitaWeaponIntField::Count)];
    float floatFields[static_cast<uint32_t>(VitaWeaponFloatField::Count)];
    float movementVectors[8][3];
    float parallelBounce[kVitaWeaponBounceCurveCount];
    float perpendicularBounce[kVitaWeaponBounceCurveCount];
    float projectileColor[3];
    float locationDamageMultipliers[kVitaWeaponLocationDamageCount];

    bool SourceComplete() const {
        return sourceSchemaVersion == kVitaWeaponVersionSource
            && sourceLayoutVersion == kVitaWeaponSourceLayoutVersion
            && (sourceFlags & kVitaWeaponSourceRequiredFlags)
                == kVitaWeaponSourceRequiredFlags;
    }

    const char* ViewAnimationName(uint32_t index) const {
        return index < kVitaWeaponAnimationCount
                && viewAnimationNames[index][0] != '\0'
            ? viewAnimationNames[index] : nullptr;
    }
};

static_assert(std::is_standard_layout_v<VitaWeaponRecordV5>);
static_assert(std::is_trivially_copyable_v<VitaWeaponRecordV5>);
static_assert(offsetof(VitaWeaponRecordV5, viewModelName) == 160);
static_assert(offsetof(VitaWeaponRecordV5, sourceSchemaVersion) == 2360);

inline constexpr uint32_t kVitaWeaponRecordV5Bytes =
    static_cast<uint32_t>(sizeof(VitaWeaponRecordV5));
static_assert(kVitaWeaponRecordV5Bytes == 15376,
              "VITP v5 wire record ABI changed");

} // namespace kisak::vita
