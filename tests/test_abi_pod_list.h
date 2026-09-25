#pragma once
#define OFPS_ABI_POD_LIST(X) \
X(sizeof(OfpsVersion)) \
X(sizeof(OfpsRect)) \
X(sizeof(OfpsRectF)) \
X(sizeof(OfpsResource)) \
X(sizeof(OfpsFrameInputs)) \
X(sizeof(OfpsModelInputs)) \
X(sizeof(OfpsFeatureDesc)) \
X(sizeof(OfpsEvalResult)) \
X(sizeof(OfpsFencePoint)) \
X(sizeof(OfpsEventData)) \
X(sizeof(OfpsHostCaps)) \
X(sizeof(OfpsStatus)) \
X(sizeof(OfpsStatusRow)) \
X(sizeof(OfpsLayoutPreview)) \
X(sizeof(OfpsSettingValue)) \
X(sizeof(OfpsSettingsValues)) \
X(sizeof(OfpsVisibleIf)) \
X(sizeof(OfpsSettingDesc)) \
X(offsetof(OfpsResource, res)) \
X(offsetof(OfpsResource, rect)) \
X(offsetof(OfpsResource, subresource)) \
X(offsetof(OfpsFrameInputs, color)) \
X(offsetof(OfpsFrameInputs, codecInputs)) \
X(offsetof(OfpsFrameInputs, colorDomain)) \
X(offsetof(OfpsModelInputs, mvScaleX)) \
X(offsetof(OfpsModelInputs, height)) \
X(offsetof(OfpsFeatureDesc, adapterLuid)) \
X(offsetof(OfpsFencePoint, value)) \
X(offsetof(OfpsEventData, text)) \
X(offsetof(OfpsStatus, evaluations)) \
X(offsetof(OfpsStatus, reason)) \
X(offsetof(OfpsStatusRow, severity)) \
X(offsetof(OfpsLayoutPreview, diagnosticFlags)) \
X(offsetof(OfpsSettingsValues, explicitMask)) \
X(offsetof(OfpsSettingDesc, defaultValue)) \
X(offsetof(OfpsSettingDesc, customWidget))
