// Copyright CesiumGS, Inc. and Contributors

#include "CesiumGeoreference.h"
#include "Engine/World.h"
#include "Engine/LevelStreaming.h"
#include "Misc/PackageName.h"
#include "CesiumGeospatial/Transforms.h"
#include "CesiumUtility/Math.h"
#include "CesiumGeoreferenceable.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "Math/Vector.h"
#include "Math/Rotator.h"
#include "Math/RotationTranslationMatrix.h"
#include "Math/Matrix.h"
#include "CesiumTransforms.h"
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <optional>

#if WITH_EDITOR
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Slate/SceneViewport.h"
#endif

/*static*/ ACesiumGeoreference* ACesiumGeoreference::GetDefaultForActor(AActor* Actor) {
	ACesiumGeoreference* pGeoreference = FindObject<ACesiumGeoreference>(Actor->GetLevel(), TEXT("CesiumGeoreferenceDefault"));
	if (!pGeoreference) {
		FActorSpawnParameters spawnParameters;
		spawnParameters.Name = TEXT("CesiumGeoreferenceDefault");
		spawnParameters.OverrideLevel = Actor->GetLevel();
		pGeoreference = Actor->GetWorld()->SpawnActor<ACesiumGeoreference>(spawnParameters);
	}
	return pGeoreference;
}

ACesiumGeoreference::ACesiumGeoreference()
{
	PrimaryActorTick.bCanEverTick = true;
}

void ACesiumGeoreference::PlaceGeoreferenceOriginHere() {
	// TODO: check that we are in editor mode, and not play-in-editor mode
	// TODO: should we just assume origin rebasing isn't happening since this is only editor-mode?
	// Or are we even sure this is only for editor-mode?
#if WITH_EDITOR
	glm::dmat4 georeferencedToEllipsoidCenteredTransform = this->GetGeoreferencedToEllipsoidCenteredTransform();

	FViewport* pViewport = GEditor->GetActiveViewport();
	FViewportClient* pViewportClient = pViewport->GetClient();
	FEditorViewportClient* pEditorViewportClient = static_cast<FEditorViewportClient*>(pViewportClient);

	FRotationTranslationMatrix fCameraTransform(
		pEditorViewportClient->GetViewRotation(), 
		pEditorViewportClient->GetViewLocation()
	);
	const FIntVector& originLocation = this->GetWorld()->OriginLocation;

	// TODO: optimize this, only need to transform the front direction and translation

	// camera local space to Unreal absolute world
	glm::dmat4 cameraToAbsolute(
		glm::dvec4(fCameraTransform.M[0][0], fCameraTransform.M[0][1], fCameraTransform.M[0][2], 0.0),
		glm::dvec4(fCameraTransform.M[1][0], fCameraTransform.M[1][1], fCameraTransform.M[1][2], 0.0),
		glm::dvec4(fCameraTransform.M[2][0], fCameraTransform.M[2][1], fCameraTransform.M[2][2], 0.0),
		glm::dvec4(
			static_cast<double>(fCameraTransform.M[3][0]) + static_cast<double>(originLocation.X),
			static_cast<double>(fCameraTransform.M[3][1]) + static_cast<double>(originLocation.Y),
			static_cast<double>(fCameraTransform.M[3][2]) + static_cast<double>(originLocation.Z),
			1.0
		)
	);

	// camera local space to ECEF
	glm::dmat4 cameraToECEF = georeferencedToEllipsoidCenteredTransform * CesiumTransforms::scaleToCesium * CesiumTransforms::unrealToOrFromCesium * cameraToAbsolute;

	// Long/Lat/Height camera location (also our new target georeference origin)
	std::optional<CesiumGeospatial::Cartographic> targetGeoreferenceOrigin = CesiumGeospatial::Ellipsoid::WGS84.cartesianToCartographic(cameraToECEF[3]);

	if (!targetGeoreferenceOrigin) {
		// TODO: should there be some other default behavior here? This only happens when the location is too close to the center of the Earth.
		return;
	}

	this->SetGeoreferenceOrigin(
		CesiumUtility::Math::radiansToDegrees((*targetGeoreferenceOrigin).longitude),
		CesiumUtility::Math::radiansToDegrees((*targetGeoreferenceOrigin).latitude),
		(*targetGeoreferenceOrigin).height
	);

	// get the updated ECEF to georeferenced transform
	glm::dmat4 ellipsoidCenteredToGeoreferenced = this->GetEllipsoidCenteredToGeoreferencedTransform();

	glm::dmat4 absoluteToRelativeWorld(
		glm::dvec4(1.0, 0.0, 0.0, 0.0),
		glm::dvec4(0.0, 1.0, 0.0, 0.0),
		glm::dvec4(0.0, 0.0, 1.0, 0.0),
		glm::dvec4(-originLocation.X, -originLocation.Y, -originLocation.Z, 1.0)
	);

	// TODO: check for degeneracy ?
	glm::dmat4 newCameraTransform = absoluteToRelativeWorld * CesiumTransforms::unrealToOrFromCesium * CesiumTransforms::scaleToUnrealWorld * ellipsoidCenteredToGeoreferenced * cameraToECEF;
	glm::dvec3 cameraFront = glm::normalize(newCameraTransform[0]);
	glm::dvec3 cameraRight = glm::normalize(glm::cross(glm::dvec3(0.0, 0.0, 1.0), cameraFront));
	glm::dvec3 cameraUp = glm::normalize(glm::cross(cameraFront, cameraRight));

	pEditorViewportClient->SetViewRotation(
		FMatrix(
			FVector(cameraFront.x, cameraFront.y, cameraFront.z),
			FVector(cameraRight.x, cameraRight.y, cameraRight.z),
			FVector(cameraUp.x, cameraUp.y, cameraUp.z),
			FVector(0.0f, 0.0f, 0.0f)
		).Rotator()
	);	
	pEditorViewportClient->SetViewLocation(FVector(-originLocation.X, -originLocation.Y, -originLocation.Z));
#endif
}

void ACesiumGeoreference::CheckForNewSubLevels() {
	const TArray<ULevelStreaming*>& streamedLevels = this->GetWorld()->GetStreamingLevels();
	// check all levels to see if any are new
	for (ULevelStreaming* streamedLevel : streamedLevels) {
		FString levelName = FPackageName::GetShortName(streamedLevel->GetWorldAssetPackageName());
		levelName.RemoveFromStart(this->GetWorld()->StreamingLevelsPrefix);
		// check the known levels to see if this one is new
		bool found = false;
		for (FCesiumSubLevel& subLevel : this->CesiumSubLevels) {
			if (levelName.Equals(subLevel.LevelName)) {
				found = true;
				break;
			}
		}
		
		if (!found) {
			// add this level to the known streaming levels
			this->CesiumSubLevels.Add(FCesiumSubLevel{
				levelName,
				OriginLongitude,
				OriginLatitude,
				OriginHeight,
				1000.0 // TODO: figure out better default radius
			});
		}
	}
}

void ACesiumGeoreference::JumpToCurrentLevel() {
	if (this->CurrentLevelIndex < 0 || this->CurrentLevelIndex >= this->CesiumSubLevels.Num()) {
		return;
	}

	const FCesiumSubLevel& currentLevel = this->CesiumSubLevels[this->CurrentLevelIndex];

	this->SetGeoreferenceOrigin(currentLevel.LevelLongitude, currentLevel.LevelLatitude, currentLevel.LevelHeight);
}

void ACesiumGeoreference::SetGeoreferenceOrigin(double targetLongitude, double targetLatitude, double targetHeight) {
	this->OriginLongitude = targetLongitude;
	this->OriginLatitude = targetLatitude;
	this->OriginHeight = targetHeight;

	this->UpdateGeoreference();
}

void ACesiumGeoreference::InaccurateSetGeoreferenceOrigin(float targetLongitude, float targetLatitude, float targetHeight) {
	this->SetGeoreferenceOrigin(targetLongitude, targetLatitude, targetHeight);
}

glm::dmat4x4 ACesiumGeoreference::GetGeoreferencedToEllipsoidCenteredTransform() const {
	if (this->OriginPlacement == EOriginPlacement::TrueOrigin) {
		return glm::dmat4(1.0);
	}

	glm::dvec3 center(0.0, 0.0, 0.0);

	if (this->OriginPlacement == EOriginPlacement::BoundingVolumeOrigin) {
		// TODO: it'd be better to compute the union of the bounding volumes and then use the union's center,
		//       rather than averaging the centers.
		size_t numberOfPositions = 0;

		for (const TWeakInterfacePtr<ICesiumGeoreferenceable> pObject : this->_georeferencedObjects) {
			if (pObject.IsValid() && pObject->IsBoundingVolumeReady()) {
				std::optional<Cesium3DTiles::BoundingVolume> bv = pObject->GetBoundingVolume();
				if (bv) {
					center += Cesium3DTiles::getBoundingVolumeCenter(*bv);
					++numberOfPositions;
				}
			}
		}

		if (numberOfPositions > 0) {
			center /= numberOfPositions;
		}
	}
	else if (this->OriginPlacement == EOriginPlacement::CartographicOrigin) {
		const CesiumGeospatial::Ellipsoid& ellipsoid = CesiumGeospatial::Ellipsoid::WGS84;
		center = ellipsoid.cartographicToCartesian(CesiumGeospatial::Cartographic::fromDegrees(this->OriginLongitude, this->OriginLatitude, this->OriginHeight));
	}

	if (this->AlignTilesetUpWithZ) {
		return CesiumGeospatial::Transforms::eastNorthUpToFixedFrame(center);
	} else {
		return glm::translate(glm::dmat4(1.0), center);
	}
}

glm::dmat4x4 ACesiumGeoreference::GetEllipsoidCenteredToGeoreferencedTransform() const {
	return glm::affineInverse(this->GetGeoreferencedToEllipsoidCenteredTransform());
}

void ACesiumGeoreference::AddGeoreferencedObject(ICesiumGeoreferenceable* Object)
{
	this->_georeferencedObjects.Add(*Object);

	// If this object is an Actor or UActorComponent, make sure it ticks _after_ the CesiumGeoreference.
	AActor* pActor = Cast<AActor>(Object);
	UActorComponent* pActorComponent = Cast<UActorComponent>(Object);
	if (pActor) {
		pActor->AddTickPrerequisiteActor(this);
	} else if (pActorComponent) {
		pActorComponent->AddTickPrerequisiteActor(this);
	}
	
	this->UpdateGeoreference();
}

// Called when the game starts or when spawned
void ACesiumGeoreference::BeginPlay()
{
	Super::BeginPlay();
	
	if (this->KeepWorldOriginNearCamera && !this->WorldOriginCamera) {
		// Find the first player's camera manager
		APlayerController* pPlayerController = this->GetWorld()->GetFirstPlayerController();
		if (pPlayerController) {
			this->WorldOriginCamera = pPlayerController->PlayerCameraManager;
		}
	}
}

void ACesiumGeoreference::OnConstruction(const FTransform& Transform)
{}

void ACesiumGeoreference::UpdateGeoreference()
{
	glm::dmat4 transform = this->GetEllipsoidCenteredToGeoreferencedTransform();
	for (TWeakInterfacePtr<ICesiumGeoreferenceable> pObject : this->_georeferencedObjects) {
		if (pObject.IsValid()) {
			pObject->UpdateGeoreferenceTransform(transform);
		}
	}
}

#if WITH_EDITOR
void ACesiumGeoreference::PostEditChangeProperty(FPropertyChangedEvent& event)
{
	Super::PostEditChangeProperty(event);

	if (!event.Property) {
		return;
	}

	FName propertyName = event.Property->GetFName();

	if (
		propertyName == GET_MEMBER_NAME_CHECKED(ACesiumGeoreference, OriginPlacement) ||
		propertyName == GET_MEMBER_NAME_CHECKED(ACesiumGeoreference, OriginLongitude) ||
		propertyName == GET_MEMBER_NAME_CHECKED(ACesiumGeoreference, OriginLatitude) ||
		propertyName == GET_MEMBER_NAME_CHECKED(ACesiumGeoreference, OriginHeight) ||
		propertyName == GET_MEMBER_NAME_CHECKED(ACesiumGeoreference, AlignTilesetUpWithZ)
	) {
		this->UpdateGeoreference();
		return;
	} else if (
		propertyName == GET_MEMBER_NAME_CHECKED(ACesiumGeoreference, CurrentLevelIndex)
	) {
		this->JumpToCurrentLevel();
	} else if (
		propertyName == GET_MEMBER_NAME_CHECKED(ACesiumGeoreference, CesiumSubLevels)
	) {
		
	}
}
#endif

// Called every frame
void ACesiumGeoreference::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (this->KeepWorldOriginNearCamera && this->WorldOriginCamera) {
		const FMinimalViewInfo& pov = this->WorldOriginCamera->ViewTarget.POV;
		const FVector& cameraLocation = pov.Location;

		// TODO: decide whether to origin rebase within static scenes, maybe it should at least be optional
		// TODO: If KeepWorldOriginNearCamera is on and we play-in-editor and then exit back into the editor,
		// the editor viewport camera always goes back to the origin. This might be super annoying to users.

		// WIP check if the camera is near any unloaded levels we might want to stream in. If we leave the radius,
		// we also need to unload them. 
		// TODO: should this check happen 
		
		const FIntVector& originLocation = this->GetWorld()->OriginLocation;

		glm::dvec4 cameraAbsolute(
			static_cast<double>(cameraLocation.X) + static_cast<double>(originLocation.X),
			static_cast<double>(cameraLocation.Y) + static_cast<double>(originLocation.Y),
			static_cast<double>(cameraLocation.Z) + static_cast<double>(originLocation.Z),
			1.0
		);

		glm::dmat4 georeferencedToECEF = this->GetGeoreferencedToEllipsoidCenteredTransform();

		glm::dvec3 cameraECEF = georeferencedToECEF * CesiumTransforms::scaleToCesium * CesiumTransforms::unrealToOrFromCesium * cameraAbsolute;
		
		const TArray<ULevelStreaming*>& streamedLevels = this->GetWorld()->GetStreamingLevels();
		for (ULevelStreaming* streamedLevel : streamedLevels) {
			FString levelName = FPackageName::GetShortName(streamedLevel->GetWorldAssetPackageName());
			levelName.RemoveFromStart(this->GetWorld()->StreamingLevelsPrefix);
			// TODO: maybe we should precalculate the level ECEF from level long/lat/height
			// TODO: consider the case where we're intersecting multiple level radii
			for (const FCesiumSubLevel& level : this->CesiumSubLevels) {
				// if this is a known level, we need to tell it whether or not it should be loaded
				if (levelName.Equals(level.LevelName)) {
					glm::dvec3 levelECEF = CesiumGeospatial::Ellipsoid::WGS84.cartographicToCartesian(
						CesiumGeospatial::Cartographic::fromDegrees(
							level.LevelLongitude, level.LevelLatitude, level.LevelHeight
						)
					);

					if (glm::length(levelECEF - cameraECEF) < level.LoadRadius) {
						streamedLevel->SetShouldBeLoaded(true);
						this->_jumpToLevel(level);
					} else {
						streamedLevel->SetShouldBeLoaded(false);
					}

					break;
				}
			}
		}

		if (!cameraLocation.Equals(FVector(0.0f, 0.0f, 0.0f), this->MaximumWorldOriginDistanceFromCamera)) {
			// Camera has moved too far from the origin, move the origin.
			this->GetWorld()->SetNewWorldOrigin(FIntVector(
				static_cast<int32>(cameraLocation.X) + static_cast<int32>(originLocation.X),
				static_cast<int32>(cameraLocation.Y) + static_cast<int32>(originLocation.Y),
				static_cast<int32>(cameraLocation.Z) + static_cast<int32>(originLocation.Z)
			));
		}
	}
}

void ACesiumGeoreference::_jumpToLevel(const FCesiumSubLevel& level) {
	this->SetGeoreferenceOrigin(level.LevelLongitude, level.LevelLatitude, level.LevelHeight);
}

