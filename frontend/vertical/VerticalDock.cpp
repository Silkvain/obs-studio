/******************************************************************************
    Siegfried: Vertical canvas dock (preview, scenes, sources).

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "VerticalDock.hpp"
#include "VerticalCanvas.hpp"

#include <OBSApp.hpp>
#include <utility/display-helpers.hpp>
#include <qt-wrappers.hpp>

#include <obs-frontend-api.h>
#include <graphics/matrix4.h>
#include <graphics/vec4.h>

#include <QComboBox>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

#include "moc_VerticalDock.cpp"

namespace {

/* Colours are 0xAABBGGRR (vec4_from_rgba) */
constexpr uint32_t kSelectColor = 0xFF4DA33F;   /* Siegfried green */
constexpr uint32_t kSafeZoneColor = 0x400000FF; /* translucent red */
constexpr uint32_t kSafeLineColor = 0xA00000FF;
constexpr float kHandlePx = 10.0f;

/* Phone app overlays (TikTok / Shorts / Reels): title bar on top, caption
 * and buttons at the bottom, like/comment/share column on the right. */
constexpr float kSafeTop = 0.10f;
constexpr float kSafeBottom = 0.20f;
constexpr float kSafeRight = 0.14f;

void FillRect(float x, float y, float w, float h, uint32_t color)
{
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *param = gs_effect_get_param_by_name(solid, "color");
	vec4 c;
	vec4_from_rgba(&c, color);
	gs_effect_set_vec4(param, &c);

	gs_matrix_push();
	gs_matrix_translate3f(x, y, 0.0f);
	gs_matrix_scale3f(w, h, 1.0f);
	while (gs_effect_loop(solid, "Solid")) {
		gs_draw_sprite(nullptr, 0, 1, 1);
	}
	gs_matrix_pop();
}

void DrawLine(const vec3 &a, const vec3 &b, float thickness, uint32_t color)
{
	float dx = b.x - a.x, dy = b.y - a.y;
	float len = std::sqrt(dx * dx + dy * dy);
	if (len <= 0.0f) {
		return;
	}
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *param = gs_effect_get_param_by_name(solid, "color");
	vec4 c;
	vec4_from_rgba(&c, color);
	gs_effect_set_vec4(param, &c);

	gs_matrix_push();
	gs_matrix_translate3f(a.x, a.y, 0.0f);
	gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, std::atan2(dy, dx));
	gs_matrix_translate3f(0.0f, -thickness / 2.0f, 0.0f);
	gs_matrix_scale3f(len, thickness, 1.0f);
	while (gs_effect_loop(solid, "Solid")) {
		gs_draw_sprite(nullptr, 0, 1, 1);
	}
	gs_matrix_pop();
}

void BoxCorners(obs_sceneitem_t *item, vec3 out[4])
{
	matrix4 m;
	obs_sceneitem_get_box_transform(item, &m);
	const float pts[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
	for (int i = 0; i < 4; i++) {
		vec3 p;
		vec3_set(&p, pts[i][0], pts[i][1], 0.0f);
		vec3_transform(&out[i], &p, &m);
	}
}

vec2 BoxCenter(obs_sceneitem_t *item)
{
	vec3 c[4];
	BoxCorners(item, c);
	vec2 r;
	vec2_set(&r, (c[0].x + c[2].x) / 2.0f, (c[0].y + c[2].y) / 2.0f);
	return r;
}

bool PointInItem(obs_sceneitem_t *item, const vec2 &pos)
{
	matrix4 m, inv;
	obs_sceneitem_get_box_transform(item, &m);
	matrix4_inv(&inv, &m);
	vec3 p, local;
	vec3_set(&p, pos.x, pos.y, 0.0f);
	vec3_transform(&local, &p, &inv);
	return local.x >= 0.0f && local.x <= 1.0f && local.y >= 0.0f && local.y <= 1.0f;
}

OBSScene SceneOf(VerticalCanvas *vc)
{
	OBSSource s = vc->CurrentScene();
	return obs_scene_from_source(s);
}

} // namespace

/* ------------------------------------------------------------------------ */
/* Preview                                                                  */

VerticalPreview::VerticalPreview(QWidget *parent, VerticalCanvas *vc_) : OBSQTDisplay(parent), vc(vc_)
{
	setFocusPolicy(Qt::ClickFocus);
	setMouseTracking(false);
	setMinimumSize(120, 200);

	auto addDrawCallback = [this]() {
		obs_display_add_draw_callback(GetDisplay(), Render, this);
	};
	connect(this, &OBSQTDisplay::DisplayCreated, this, addDrawCallback);
}

VerticalPreview::~VerticalPreview()
{
	if (GetDisplay()) {
		obs_display_remove_draw_callback(GetDisplay(), Render, this);
	}
}

void VerticalPreview::Render(void *data, uint32_t cx, uint32_t cy)
{
	auto *self = static_cast<VerticalPreview *>(data);
	OBSCanvasAutoRelease canvas = self->vc->GetCanvas();
	uint32_t bw = 0, bh = 0;
	if (!canvas || !obs_canvas_get_base_size(canvas, &bw, &bh) || !cx || !cy) {
		return;
	}

	int x, y;
	float scale;
	GetScaleAndCenterPos(int(bw), int(bh), int(cx), int(cy), x, y, scale);
	int vw = int(scale * float(bw));
	int vh = int(scale * float(bh));

	gs_projection_push();
	gs_viewport_push();
	gs_set_viewport(x, y, vw, vh);
	gs_ortho(0.0f, float(bw), 0.0f, float(bh), -100.0f, 100.0f);

	FillRect(0.0f, 0.0f, float(bw), float(bh), 0xFF000000);
	obs_canvas_render(canvas);
	gs_load_vertexbuffer(nullptr);

	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

	float px = 1.0f / std::max(scale, 0.0001f); /* one screen pixel in canvas units */

	if (self->vc->SafeZones()) {
		float top = float(bh) * kSafeTop;
		float bottom = float(bh) * kSafeBottom;
		float right = float(bw) * kSafeRight;
		FillRect(0.0f, 0.0f, float(bw), top, kSafeZoneColor);
		FillRect(0.0f, float(bh) - bottom, float(bw), bottom, kSafeZoneColor);
		FillRect(float(bw) - right, top, right, float(bh) - top - bottom, kSafeZoneColor);

		vec3 a, b;
		vec3_set(&a, 0.0f, top, 0.0f);
		vec3_set(&b, float(bw) - right, top, 0.0f);
		DrawLine(a, b, px, kSafeLineColor);
		vec3_set(&a, 0.0f, float(bh) - bottom, 0.0f);
		vec3_set(&b, float(bw) - right, float(bh) - bottom, 0.0f);
		DrawLine(a, b, px, kSafeLineColor);
		vec3_set(&a, float(bw) - right, top, 0.0f);
		vec3_set(&b, float(bw) - right, float(bh) - bottom, 0.0f);
		DrawLine(a, b, px, kSafeLineColor);
	}

	/* Selection outline and corner handles */
	OBSSourceAutoRelease sceneSource = obs_canvas_get_channel(canvas, 0);
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (scene) {
		struct Ctx {
			float px;
		} ctx{px};
		auto cb = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			if (!obs_sceneitem_selected(item) || !obs_sceneitem_visible(item)) {
				return true;
			}
			float p = static_cast<Ctx *>(param)->px;
			vec3 c[4];
			BoxCorners(item, c);
			for (int i = 0; i < 4; i++) {
				DrawLine(c[i], c[(i + 1) % 4], 2.0f * p, kSelectColor);
			}
			float hs = kHandlePx * p;
			for (int i = 0; i < 4; i++) {
				FillRect(c[i].x - hs / 2.0f, c[i].y - hs / 2.0f, hs, hs, kSelectColor);
			}
			return true;
		};
		obs_scene_enum_items(scene, cb, &ctx);
	}

	gs_blend_state_pop();
	gs_viewport_pop();
	gs_projection_pop();
}

bool VerticalPreview::MapToCanvas(const QPointF &widgetPos, vec2 &out, float *scaleOut) const
{
	uint32_t bw, bh;
	if (!vc->GetSize(bw, bh)) {
		return false;
	}
	QSize px = GetPixelSize(const_cast<VerticalPreview *>(this));
	if (px.isEmpty()) {
		return false;
	}
	int x, y;
	float scale;
	GetScaleAndCenterPos(int(bw), int(bh), px.width(), px.height(), x, y, scale);
	qreal dpr = devicePixelRatioF();
	vec2_set(&out, float((widgetPos.x() * dpr - x) / scale), float((widgetPos.y() * dpr - y) / scale));
	if (scaleOut) {
		*scaleOut = float(scale / dpr); /* canvas units -> widget units */
	}
	return true;
}

OBSSceneItem VerticalPreview::ItemAt(const vec2 &pos) const
{
	OBSScene scene = SceneOf(vc);
	if (!scene) {
		return nullptr;
	}
	struct Ctx {
		vec2 pos;
		OBSSceneItem hit;
	} ctx{pos, nullptr};
	/* Enumerates bottom to top, so the last hit is the topmost */
	auto cb = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
		auto *c = static_cast<Ctx *>(param);
		if (obs_sceneitem_visible(item) && !obs_sceneitem_locked(item) && PointInItem(item, c->pos)) {
			c->hit = item;
		}
		return true;
	};
	obs_scene_enum_items(scene, cb, &ctx);
	return ctx.hit;
}

OBSSceneItem VerticalPreview::HandleAt(const vec2 &pos, float scale) const
{
	OBSSceneItem item = Selected();
	if (!item || !obs_sceneitem_visible(item)) {
		return nullptr;
	}
	float radius = kHandlePx / std::max(scale, 0.0001f);
	vec3 c[4];
	BoxCorners(item, c);
	for (const vec3 &p : c) {
		if (std::fabs(pos.x - p.x) <= radius && std::fabs(pos.y - p.y) <= radius) {
			return item;
		}
	}
	return nullptr;
}

OBSSceneItem VerticalPreview::Selected() const
{
	OBSScene scene = SceneOf(vc);
	if (!scene) {
		return nullptr;
	}
	OBSSceneItem found;
	auto cb = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
		if (obs_sceneitem_selected(item)) {
			*static_cast<OBSSceneItem *>(param) = item;
		}
		return true;
	};
	obs_scene_enum_items(scene, cb, &found);
	return found;
}

void VerticalPreview::Select(obs_sceneitem_t *target)
{
	OBSScene scene = SceneOf(vc);
	if (!scene) {
		return;
	}
	auto cb = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
		bool want = item == static_cast<obs_sceneitem_t *>(param);
		if (obs_sceneitem_selected(item) != want) {
			obs_sceneitem_select(item, want);
		}
		return true;
	};
	obs_scene_enum_items(scene, cb, target);
	emit SelectionChanged();
}

void VerticalPreview::Snap(obs_sceneitem_t *item, float tol)
{
	uint32_t bw, bh;
	if (!vc->GetSize(bw, bh)) {
		return;
	}
	vec3 c[4];
	BoxCorners(item, c);
	float minX = c[0].x, maxX = c[0].x, minY = c[0].y, maxY = c[0].y;
	for (const vec3 &p : c) {
		minX = std::min(minX, p.x);
		maxX = std::max(maxX, p.x);
		minY = std::min(minY, p.y);
		maxY = std::max(maxY, p.y);
	}

	auto best = [tol](std::initializer_list<float> diffs) {
		float pick = 0.0f, bestAbs = tol;
		for (float d : diffs) {
			if (std::fabs(d) < bestAbs) {
				bestAbs = std::fabs(d);
				pick = d;
			}
		}
		return pick;
	};
	float dx = best({-minX, float(bw) - maxX, float(bw) / 2.0f - (minX + maxX) / 2.0f});
	float dy = best({-minY, float(bh) - maxY, float(bh) / 2.0f - (minY + maxY) / 2.0f});
	if (dx != 0.0f || dy != 0.0f) {
		vec2 pos;
		obs_sceneitem_get_pos(item, &pos);
		pos.x += dx;
		pos.y += dy;
		obs_sceneitem_set_pos(item, &pos);
	}
}

void VerticalPreview::ScaleAroundCenter(obs_sceneitem_t *item, float factor)
{
	vec2 c0 = BoxCenter(item);
	if (obs_sceneitem_get_bounds_type(item) != OBS_BOUNDS_NONE) {
		vec2 b;
		obs_sceneitem_get_bounds(item, &b);
		vec2_mulf(&b, &b, factor);
		obs_sceneitem_set_bounds(item, &b);
	} else {
		vec2 s;
		obs_sceneitem_get_scale(item, &s);
		vec2_mulf(&s, &s, factor);
		obs_sceneitem_set_scale(item, &s);
	}
	vec2 c1 = BoxCenter(item);
	vec2 pos;
	obs_sceneitem_get_pos(item, &pos);
	pos.x += c0.x - c1.x;
	pos.y += c0.y - c1.y;
	obs_sceneitem_set_pos(item, &pos);
}

void VerticalPreview::mousePressEvent(QMouseEvent *event)
{
	OBSQTDisplay::mousePressEvent(event);
	if (event->button() != Qt::LeftButton) {
		return;
	}
	vec2 pos;
	float scale;
	if (!MapToCanvas(event->position(), pos, &scale)) {
		return;
	}

	dragStart = pos;
	if (OBSSceneItem item = HandleAt(pos, scale)) {
		mode = Mode::Scale;
		dragItem = item;
		center = BoxCenter(item);
		float dx = pos.x - center.x, dy = pos.y - center.y;
		startDistance = std::max(1.0f, std::sqrt(dx * dx + dy * dy));
		obs_sceneitem_get_scale(item, &itemStartScale);
		obs_sceneitem_get_bounds(item, &itemStartBounds);
		obs_sceneitem_get_pos(item, &itemStartPos);
		return;
	}

	OBSSceneItem item = ItemAt(pos);
	Select(item);
	if (item) {
		mode = Mode::Move;
		dragItem = item;
		obs_sceneitem_get_pos(item, &itemStartPos);
	} else {
		mode = Mode::None;
		dragItem = nullptr;
	}
}

void VerticalPreview::mouseMoveEvent(QMouseEvent *event)
{
	OBSQTDisplay::mouseMoveEvent(event);
	if (mode == Mode::None || !dragItem) {
		return;
	}
	vec2 pos;
	float scale;
	if (!MapToCanvas(event->position(), pos, &scale)) {
		return;
	}
	bool snap = !(event->modifiers() & Qt::ControlModifier);

	if (mode == Mode::Move) {
		vec2 p;
		vec2_set(&p, itemStartPos.x + pos.x - dragStart.x, itemStartPos.y + pos.y - dragStart.y);
		obs_sceneitem_set_pos(dragItem, &p);
		if (snap) {
			Snap(dragItem, 12.0f / std::max(scale, 0.0001f));
		}
		return;
	}

	/* Scale: uniform, around the item's centre */
	float dx = pos.x - center.x, dy = pos.y - center.y;
	float factor = std::max(0.02f, std::sqrt(dx * dx + dy * dy) / startDistance);
	obs_sceneitem_set_pos(dragItem, &itemStartPos);
	if (obs_sceneitem_get_bounds_type(dragItem) != OBS_BOUNDS_NONE) {
		obs_sceneitem_set_bounds(dragItem, &itemStartBounds);
	} else {
		obs_sceneitem_set_scale(dragItem, &itemStartScale);
	}
	ScaleAroundCenter(dragItem, factor);
}

void VerticalPreview::mouseReleaseEvent(QMouseEvent *event)
{
	OBSQTDisplay::mouseReleaseEvent(event);
	if (event->button() == Qt::LeftButton) {
		mode = Mode::None;
		dragItem = nullptr;
	}
}

void VerticalPreview::wheelEvent(QWheelEvent *event)
{
	OBSSceneItem item = Selected();
	if (!item) {
		OBSQTDisplay::wheelEvent(event);
		return;
	}
	float steps = float(event->angleDelta().y()) / 120.0f;
	if (steps != 0.0f) {
		ScaleAroundCenter(item, std::pow(1.05f, steps));
	}
	event->accept();
}

void VerticalPreview::keyPressEvent(QKeyEvent *event)
{
	OBSSceneItem item = Selected();
	if (!item) {
		OBSQTDisplay::keyPressEvent(event);
		return;
	}
	float step = (event->modifiers() & Qt::ShiftModifier) ? 10.0f : 1.0f;
	vec2 pos;
	obs_sceneitem_get_pos(item, &pos);
	switch (event->key()) {
	case Qt::Key_Left:
		pos.x -= step;
		break;
	case Qt::Key_Right:
		pos.x += step;
		break;
	case Qt::Key_Up:
		pos.y -= step;
		break;
	case Qt::Key_Down:
		pos.y += step;
		break;
	case Qt::Key_Delete:
	case Qt::Key_Backspace:
		obs_sceneitem_remove(item);
		emit SelectionChanged();
		return;
	default:
		OBSQTDisplay::keyPressEvent(event);
		return;
	}
	obs_sceneitem_set_pos(item, &pos);
}

void VerticalPreview::contextMenuEvent(QContextMenuEvent *event)
{
	vec2 pos;
	if (!MapToCanvas(event->pos(), pos)) {
		return;
	}
	OBSSceneItem item = ItemAt(pos);
	if (!item) {
		item = Selected();
	}
	if (!item) {
		return;
	}
	Select(item);

	QMenu menu(this);
	auto fit = [&](const QString &label, VerticalCanvas::Fit mode) {
		menu.addAction(label, this, [item, mode] { VerticalCanvas::FitItem(item, mode); });
	};
	fit(QTStr("Vertical.Fit.Width"), VerticalCanvas::Fit::Width);
	fit(QTStr("Vertical.Fit.Inside"), VerticalCanvas::Fit::Inside);
	fit(QTStr("Vertical.Fit.Fill"), VerticalCanvas::Fit::Fill);
	fit(QTStr("Vertical.Fit.Center"), VerticalCanvas::Fit::Center);
	fit(QTStr("Vertical.Fit.Reset"), VerticalCanvas::Fit::Reset);
	menu.addSeparator();
	menu.addAction(QTStr("Vertical.Order.Front"), this,
		       [item] { obs_sceneitem_set_order(item, OBS_ORDER_MOVE_TOP); });
	menu.addAction(QTStr("Vertical.Order.Back"), this,
		       [item] { obs_sceneitem_set_order(item, OBS_ORDER_MOVE_BOTTOM); });
	menu.addSeparator();
	menu.addAction(QTStr("Vertical.Hide"), this, [item] { obs_sceneitem_set_visible(item, false); });
	menu.addAction(QTStr("Vertical.RemoveSource"), this, [this, item] {
		obs_sceneitem_remove(item);
		emit SelectionChanged();
	});
	menu.exec(event->globalPos());
}

/* ------------------------------------------------------------------------ */
/* Dock                                                                     */

static QToolButton *SmallButton(QWidget *parent, const QString &text, const QString &tip)
{
	QToolButton *b = new QToolButton(parent);
	b->setText(text);
	b->setToolTip(tip);
	b->setAutoRaise(true);
	return b;
}

VerticalDock::VerticalDock(QWidget *parent, VerticalCanvas *vc_) : QWidget(parent), vc(vc_)
{
	preview = new VerticalPreview(this, vc);
	preview->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

	scenes = new QComboBox(this);
	scenes->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	scenes->setToolTip(QTStr("Vertical.SceneTooltip"));
	QToolButton *addScene = SmallButton(this, QStringLiteral("+"), QTStr("Vertical.AddScene"));
	QToolButton *removeScene = SmallButton(this, QStringLiteral("−"), QTStr("Vertical.RemoveScene"));
	sceneMenu = SmallButton(this, QStringLiteral("⋯"), QTStr("Vertical.Options"));

	QHBoxLayout *sceneRow = new QHBoxLayout();
	sceneRow->setContentsMargins(0, 0, 0, 0);
	sceneRow->setSpacing(2);
	sceneRow->addWidget(scenes, 1);
	sceneRow->addWidget(addScene);
	sceneRow->addWidget(removeScene);
	sceneRow->addWidget(sceneMenu);

	sources = new QListWidget(this);
	sources->setMaximumHeight(150);
	sources->setSelectionMode(QAbstractItemView::SingleSelection);

	addSource = SmallButton(this, QTStr("Vertical.AddSource"), QTStr("Vertical.AddSourceTooltip"));
	QToolButton *removeSource = SmallButton(this, QStringLiteral("−"), QTStr("Vertical.RemoveSource"));
	QToolButton *up = SmallButton(this, QStringLiteral("↑"), QTStr("Vertical.Order.Up"));
	QToolButton *down = SmallButton(this, QStringLiteral("↓"), QTStr("Vertical.Order.Down"));
	sizeLabel = new QLabel(this);
	sizeLabel->setStyleSheet("color: palette(placeholder-text);");

	QHBoxLayout *sourceRow = new QHBoxLayout();
	sourceRow->setContentsMargins(0, 0, 0, 0);
	sourceRow->setSpacing(2);
	sourceRow->addWidget(addSource);
	sourceRow->addWidget(removeSource);
	sourceRow->addWidget(up);
	sourceRow->addWidget(down);
	sourceRow->addStretch(1);
	sourceRow->addWidget(sizeLabel);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(4, 4, 4, 4);
	layout->setSpacing(4);
	layout->addWidget(preview, 1);
	layout->addLayout(sceneRow);
	layout->addWidget(sources);
	layout->addLayout(sourceRow);

	connect(scenes, &QComboBox::activated, this, [this](int index) {
		OBSSource s = scenes->itemData(index).value<OBSSource>();
		if (s) {
			vc->SetCurrentScene(s);
		}
	});
	connect(addScene, &QToolButton::clicked, this, &VerticalDock::AddScene);
	connect(removeScene, &QToolButton::clicked, this, &VerticalDock::RemoveScene);
	connect(sceneMenu, &QToolButton::clicked, this, &VerticalDock::ShowSceneMenu);
	connect(addSource, &QToolButton::clicked, this, &VerticalDock::ShowAddMenu);
	connect(removeSource, &QToolButton::clicked, this, [this] {
		if (OBSSceneItem item = SelectedItem()) {
			obs_sceneitem_remove(item);
		}
	});
	connect(up, &QToolButton::clicked, this, [this] {
		if (OBSSceneItem item = SelectedItem()) {
			obs_sceneitem_set_order(item, OBS_ORDER_MOVE_UP);
		}
	});
	connect(down, &QToolButton::clicked, this, [this] {
		if (OBSSceneItem item = SelectedItem()) {
			obs_sceneitem_set_order(item, OBS_ORDER_MOVE_DOWN);
		}
	});

	connect(sources, &QListWidget::currentRowChanged, this, [this](int) {
		if (updating) {
			return;
		}
		QListWidgetItem *li = sources->currentItem();
		OBSSceneItem target = li ? li->data(Qt::UserRole).value<OBSSceneItem>() : nullptr;
		OBSScene scene = SceneOf(vc);
		if (!scene) {
			return;
		}
		auto cb = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			bool want = item == static_cast<obs_sceneitem_t *>(param);
			if (obs_sceneitem_selected(item) != want) {
				obs_sceneitem_select(item, want);
			}
			return true;
		};
		obs_scene_enum_items(scene, cb, target.Get());
	});
	connect(sources, &QListWidget::itemChanged, this, [this](QListWidgetItem *li) {
		if (updating) {
			return;
		}
		OBSSceneItem item = li->data(Qt::UserRole).value<OBSSceneItem>();
		if (item) {
			obs_sceneitem_set_visible(item, li->checkState() == Qt::Checked);
		}
	});

	connect(preview, &VerticalPreview::SelectionChanged, this, &VerticalDock::QueueRefreshSources);
	connect(vc, &VerticalCanvas::ScenesChanged, this, &VerticalDock::RefreshScenes);
	connect(vc, &VerticalCanvas::CurrentSceneChanged, this, &VerticalDock::ConnectScene);
	connect(vc, &VerticalCanvas::CanvasChanged, this, &VerticalDock::ConnectScene);
	connect(vc, &VerticalCanvas::SettingsChanged, this, &VerticalDock::RefreshScenes);

	RefreshScenes();
	ConnectScene();
}

VerticalDock::~VerticalDock()
{
	sceneSignals.clear();
}

void VerticalDock::RefreshScenes()
{
	updating = true;
	scenes->clear();
	OBSSource current = vc->CurrentScene();
	for (const OBSSource &s : vc->Scenes()) {
		scenes->addItem(QString::fromUtf8(obs_source_get_name(s)), QVariant::fromValue(s));
		if (s == current) {
			scenes->setCurrentIndex(scenes->count() - 1);
		}
	}
	uint32_t cx, cy;
	sizeLabel->setText(vc->GetSize(cx, cy) ? QStringLiteral("%1×%2").arg(cx).arg(cy) : QString());
	setEnabled(vc->GetCanvas() != nullptr);
	updating = false;
}

void VerticalDock::ConnectScene()
{
	sceneSignals.clear();
	OBSSource scene = vc->CurrentScene();
	connectedScene = OBSGetWeakRef(scene);
	if (scene) {
		signal_handler_t *sh = obs_source_get_signal_handler(scene);
		for (const char *sig : {"item_add", "item_remove", "reorder", "refresh", "item_visible", "item_select",
					"item_deselect"}) {
			sceneSignals.emplace_back(sh, sig, SceneSignal, this);
		}
		sceneSignals.emplace_back(sh, "item_transform", TransformSignal, this);
	}
	RefreshScenes();
	RefreshSources();
}

void VerticalDock::SceneSignal(void *data, calldata_t *)
{
	auto *self = static_cast<VerticalDock *>(data);
	QMetaObject::invokeMethod(self, &VerticalDock::QueueRefreshSources, Qt::QueuedConnection);
	QMetaObject::invokeMethod(self->vc, &VerticalCanvas::RequestSave, Qt::QueuedConnection);
}

void VerticalDock::TransformSignal(void *data, calldata_t *)
{
	auto *self = static_cast<VerticalDock *>(data);
	QMetaObject::invokeMethod(self->vc, &VerticalCanvas::RequestSave, Qt::QueuedConnection);
}

void VerticalDock::QueueRefreshSources()
{
	if (refreshQueued) {
		return;
	}
	refreshQueued = true;
	QMetaObject::invokeMethod(
		this,
		[this] {
			refreshQueued = false;
			RefreshSources();
		},
		Qt::QueuedConnection);
}

void VerticalDock::RefreshSources()
{
	updating = true;
	sources->clear();
	OBSScene scene = SceneOf(vc);
	if (scene) {
		std::vector<OBSSceneItem> items;
		auto cb = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			static_cast<std::vector<OBSSceneItem> *>(param)->emplace_back(item);
			return true;
		};
		obs_scene_enum_items(scene, cb, &items);
		/* Top of the list = top of the picture, like OBS */
		for (auto it = items.rbegin(); it != items.rend(); ++it) {
			obs_sceneitem_t *item = *it;
			QListWidgetItem *li =
				new QListWidgetItem(QString::fromUtf8(obs_source_get_name(obs_sceneitem_get_source(item))));
			li->setFlags(li->flags() | Qt::ItemIsUserCheckable);
			li->setCheckState(obs_sceneitem_visible(item) ? Qt::Checked : Qt::Unchecked);
			li->setData(Qt::UserRole, QVariant::fromValue(OBSSceneItem(item)));
			sources->addItem(li);
			if (obs_sceneitem_selected(item)) {
				sources->setCurrentItem(li);
			}
		}
	}
	updating = false;
}

OBSSceneItem VerticalDock::SelectedItem() const
{
	QListWidgetItem *li = sources->currentItem();
	return li ? li->data(Qt::UserRole).value<OBSSceneItem>() : nullptr;
}

void VerticalDock::ShowAddMenu()
{
	OBSScene scene = SceneOf(vc);
	if (!scene) {
		return;
	}

	/* Every source with a picture; they are shared with the main canvas */
	std::vector<OBSSource> list;
	auto cb = [](void *param, obs_source_t *source) {
		if (obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) {
			static_cast<std::vector<OBSSource> *>(param)->emplace_back(source);
		}
		return true;
	};
	obs_enum_sources(cb, &list);
	std::sort(list.begin(), list.end(), [](const OBSSource &a, const OBSSource &b) {
		return QString::fromUtf8(obs_source_get_name(a))
			       .compare(QString::fromUtf8(obs_source_get_name(b)), Qt::CaseInsensitive) < 0;
	});

	QMenu menu(this);
	OBSSourceAutoRelease mainScene = obs_frontend_get_current_scene();
	if (mainScene) {
		menu.addAction(QTStr("Vertical.CopyFromMain").arg(QString::fromUtf8(obs_source_get_name(mainScene))),
			       this, [this, src = OBSSource(mainScene.Get())] { vc->CopyFromMainScene(src); });
		menu.addSeparator();
	}
	if (list.empty()) {
		menu.addAction(QTStr("Vertical.NoSources"))->setEnabled(false);
	}
	for (const OBSSource &s : list) {
		menu.addAction(QString::fromUtf8(obs_source_get_name(s)), this, [this, s] {
			OBSScene sc = SceneOf(vc);
			if (!sc) {
				return;
			}
			obs_sceneitem_t *item = obs_scene_add(sc, s);
			if (item) {
				VerticalCanvas::FitItem(item, VerticalCanvas::Fit::Width);
				preview->setFocus();
			}
		});
	}
	menu.exec(addSource->mapToGlobal(QPoint(0, addSource->height())));
}

void VerticalDock::ShowSceneMenu()
{
	QMenu menu(this);
	menu.addAction(QTStr("Vertical.RenameScene"), this, &VerticalDock::RenameScene);
	menu.addSeparator();
	QAction *linked = menu.addAction(QTStr("Vertical.Linked"));
	linked->setCheckable(true);
	linked->setChecked(vc->Linked());
	linked->setToolTip(QTStr("Vertical.LinkedTooltip"));
	connect(linked, &QAction::toggled, vc, &VerticalCanvas::SetLinked);
	QAction *safe = menu.addAction(QTStr("Vertical.SafeZones"));
	safe->setCheckable(true);
	safe->setChecked(vc->SafeZones());
	connect(safe, &QAction::toggled, vc, &VerticalCanvas::SetSafeZones);
	menu.setToolTipsVisible(true);
	menu.exec(sceneMenu->mapToGlobal(QPoint(0, sceneMenu->height())));
}

void VerticalDock::AddScene()
{
	/* Default: same name as the main scene, so they're linked */
	OBSSourceAutoRelease mainScene = obs_frontend_get_current_scene();
	QString suggestion = vc->UniqueSceneName(mainScene ? QString::fromUtf8(obs_source_get_name(mainScene))
							   : QStringLiteral("Scene"));
	bool ok = false;
	QString name = QInputDialog::getText(this, QTStr("Vertical.AddScene"), QTStr("Vertical.SceneName"),
					     QLineEdit::Normal, suggestion, &ok);
	if (!ok || name.trimmed().isEmpty()) {
		return;
	}
	OBSSource s = vc->CreateScene(name);
	if (s) {
		vc->SetCurrentScene(s);
	}
}

void VerticalDock::RemoveScene()
{
	OBSSource s = vc->CurrentScene();
	if (!s) {
		return;
	}
	if (vc->Scenes().size() <= 1) {
		QMessageBox::information(this, QTStr("Vertical.RemoveScene"), QTStr("Vertical.LastScene"));
		return;
	}
	auto answer = QMessageBox::question(
		this, QTStr("Vertical.RemoveScene"),
		QTStr("Vertical.RemoveSceneConfirm").arg(QString::fromUtf8(obs_source_get_name(s))));
	if (answer == QMessageBox::Yes) {
		vc->RemoveScene(s);
	}
}

void VerticalDock::RenameScene()
{
	OBSSource s = vc->CurrentScene();
	if (!s) {
		return;
	}
	bool ok = false;
	QString name = QInputDialog::getText(this, QTStr("Vertical.RenameScene"), QTStr("Vertical.SceneName"),
					     QLineEdit::Normal, QString::fromUtf8(obs_source_get_name(s)), &ok);
	if (ok && !vc->RenameScene(s, name)) {
		QMessageBox::warning(this, QTStr("Vertical.RenameScene"), QTStr("Vertical.NameTaken"));
	}
}
