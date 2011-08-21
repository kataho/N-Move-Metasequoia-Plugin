#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <float.h>
#include <stdio.h>
#include <stdarg.h>

#include "MQBasePlugin.h"
#include "MQ3DLib.h"
#include "MQSetting.h"

#include <vector>
#include <map>
#include <set>


#define THRESHOLD_PICK_POINT 9.0f
#define THRESHOLD_PICK_LINE 9.0f

void debuglog(MQDocument doc, const char* fmt, ...);

inline bool operator<(const MQSelectVertex& v1, const MQSelectVertex& v2) 
{
	if(v1.object != v2.object) return v1.object < v2.object;
	return v1.vertex < v2.vertex;
}


static MQCommandPlugin::EDIT_OPTION s_editoption;

enum ObjectEnumerator_SkipOption {
	OE_SKIPLOCKED = 0x1,
	OE_SKIPHIDDEN = 0x2,
	OE_APPLYEDITOPTION = 0x4,
	OE_APPLYALL = 0xff
};

class ObjectEnumerator
{
public:

	ObjectEnumerator(MQDocument doc, DWORD option = OE_APPLYALL)
	{
		m_option = option;
		m_doc = doc;
		m_objcount = doc->GetObjectCount();
		m_cur = -1;
	}
	
	void Reset() { m_cur = -1; }

	int GetIndex() { return m_cur; }

	MQObject next()
	{
		while(1)
		{
			m_cur++;
			if(m_cur >= m_objcount) return NULL;

			MQObject obj = m_doc->GetObject(m_cur);

			if(obj == NULL) continue;
			if(obj->GetVertexCount() == 0) continue;
			if((m_option & OE_SKIPLOCKED) && obj->GetLocking()) continue;
			if((m_option & OE_SKIPHIDDEN) && !obj->GetVisible()) continue;
			if((m_option & OE_APPLYEDITOPTION) && s_editoption.CurrentObjectOnly && m_cur != m_doc->GetCurrentObjectIndex()) continue;

			return obj;	
		}
	}

private:
	ObjectEnumerator() {}

	int m_cur;
	int m_objcount;

	MQDocument m_doc;
	DWORD m_option;
};

enum MQSelectElement_SelectedItemType {
	SELEL_NONE,
	SELEL_VERTEX,
	SELEL_LINE,
	SELEL_FACE,
};

class MQSelectElement
{
public:
	MQSelectElement() 
	{
		type = SELEL_NONE; 
		index_o = -1;
		index_1 = -1;
		index_2 = -1;
	}

	bool IsEmpty() { return (type == SELEL_NONE); }
	void Reset() { type = SELEL_NONE; index_o = -1; index_1 = -1; index_2 = -1; }
	void SetVertex(int o, int v) { type = SELEL_VERTEX; index_o = o; index_1 = v; index_2 = -1; }
	void SetLine(int o, int f, int l) { type = SELEL_LINE; index_o = o; index_1 = f; index_2 = l; }
	void SetFace(int o, int f) { type = SELEL_FACE; index_o = o; index_1 = f; index_2 = -1; }

	int GetType() { return type; }
	int GetObjectIndex() { return index_o; }
	int GetFaceIndex() { return index_1; }
	int GetLineIndex() { return index_2; }
	int GetVertexIndex() { return index_1; }

	bool operator!=(const MQSelectElement& a) 
	{ 
		return (type != a.type) || (index_o != a.index_o) || (index_1 != a.index_1) || (index_2 != a.index_2);
	}
	bool operator==(const MQSelectElement& a) { return !(*this != a ); }

	MQPoint GetPoint(MQDocument doc)
	{
		MQPoint p(0,0,0);
		if(IsEmpty()) return p;
		MQObject obj = doc->GetObject(index_o);
		int indices[5];
		int pcount;
		if(obj == NULL) return p;
		switch(type)
		{
		case SELEL_FACE:
			obj->GetFacePointArray(index_1,indices);
			pcount = obj->GetFacePointCount(index_1);
			for(int i=0;i<pcount;i++) p += obj->GetVertex(indices[i]);
			p /= (float)pcount;
			break;
		case SELEL_LINE:
			obj->GetFacePointArray(index_1,indices);
			pcount = obj->GetFacePointCount(index_1);
			p += obj->GetVertex(indices[index_2]);
			p += obj->GetVertex(indices[(index_2+1)%pcount]);
			p /= 2.0f;
			break;
		case SELEL_VERTEX:
			p = obj->GetVertex(index_1);
			break;
		}
		return p;
	}

	void Select(MQDocument doc)
	{
		switch(type)
		{
		case SELEL_FACE:
			doc->AddSelectFace(index_o,index_1);
			break;
		case SELEL_LINE:
			doc->AddSelectLine(index_o,index_1,index_2); 
			break;
		case SELEL_VERTEX:
			doc->AddSelectVertex(index_o,index_1);
			break;
		}
	}

	void Deselect(MQDocument doc)
	{
		switch(type)
		{
		case SELEL_FACE:
			doc->DeleteSelectFace(index_o,index_1);
			break;
		case SELEL_LINE:
			doc->DeleteSelectLine(index_o,index_1,index_2);
			break;
		case SELEL_VERTEX:
			doc->DeleteSelectVertex(index_o,index_1);
			break;
		}
	}

	bool IsSelected(MQDocument doc)
	{
		switch(type)
		{
		case SELEL_FACE:
			return doc->IsSelectFace(index_o,index_1) == TRUE;
		case SELEL_LINE:
			return doc->IsSelectLine(index_o,index_1,index_2) == TRUE;
		case SELEL_VERTEX:
			return doc->IsSelectVertex(index_o,index_1) == TRUE;
		default:
			return false;
		}
	}

private:
	int type;
	int index_o;
	int index_1;
	int index_2;
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////

class ExMovePlugin : public MQCommandPlugin
{
public:
	ExMovePlugin() 
	{
		m_highlightedelement.Reset();
		m_moved = false;
	}
	~ExMovePlugin()
	{
	}

	void GetPlugInID(DWORD *Product, DWORD *ID)
	{
		*Product = 0xabcdef0e;
		*ID      = 0x00101010;
	}

	const char *GetPlugInName(void) { return "N-Move ver.1.0.1      Copyright(C) 2007 kth"; }

	const char *EnumString(void) { return "N-Move"; }

	BOOL Initialize() { return TRUE; }
	void Exit() {}

	BOOL Activate(MQDocument doc, BOOL flag);

	BOOL OnMouseMove(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state);
	void OnDraw(MQDocument doc, MQScene scene, int width, int height);

	BOOL OnLeftButtonDown(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state);
	BOOL OnLeftButtonMove(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state);
	BOOL OnLeftButtonUp(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state);

	BOOL OnRightButtonDown(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state);

	void OnObjectModified(MQDocument doc) { refresh_edge_cache(doc); m_cache_last_camera_pos.x = FLT_MAX; }
	void OnUpdateObjectList(MQDocument doc) { m_cache_last_camera_pos.x = FLT_MAX; }




	//void OnUpdateUndo(MQDocument doc, int i1, int i2) { m_cache_last_camera_pos.x = FLT_MAX; }

private:
	void refresh_cache(MQDocument doc,MQScene scene);
	void refresh_edge_cache(MQDocument doc);
	void pick_target(MQDocument doc, MQScene scene, POINT& mousepos, MQSelectElement* elm);
	BOOL marge_vertices(MQDocument doc, MQScene scene);
	void regional_select(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state);

	MQSelectElement m_highlightedelement;

	bool m_regional_select_mode;
	bool m_moved;
	MQSelectElement m_togglereserve;

	std::vector<MQSelectVertex> m_selection;
	std::vector<MQSelectVertex> m_symmetry;
	std::map<MQSelectVertex,MQPoint> m_normalmap;

	float m_sc_dragbegin_z;
	LONG m_mouse_sc_drag_x;
	
	MQPoint m_mouse_drag;
	MQPoint m_mouse_drag_ignorey;

	MQPoint m_mouse_sc_dragbegin;

	MQPoint m_cache_last_camera_pos;
	MQScene m_cache_last_scene;
	
	std::map<int, std::vector<int> > m_cache_editable_vertices;
	std::map<int, std::vector<int> > m_cache_editable_faces;
	std::map<int, std::vector< std::vector<int> > > m_cache_edges;

	MQColor m_color_highlight;
};

static  ExMovePlugin s_plugin;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////


static void find_faces_contains_vertex(MQObject obj, int vindex, std::vector<int>& out)
{
	out.reserve(8);
	int fcount = obj->GetFaceCount();
	for(int f = 0; f < fcount; f++)
	{
		int indices[5] = {-1,-1,-1,-1,-1};
		obj->GetFacePointArray(f,indices);
		
		for(int i = 0; indices[i] != -1; i++)
		{
			if(vindex == indices[i]) { out.push_back(f); break; }
		}
	}
}


static void get_selection(MQDocument doc,MQScene scene,std::vector<MQSelectVertex>& out)
{
	int indices[4];

	int obj_count = doc->GetObjectCount();

	std::set<MQSelectVertex> tmp;

	// オブジェクトの数だけ繰り返し
	ObjectEnumerator objenum(doc);
	for(MQObject obj = NULL; (obj = objenum.next()) != NULL;)
	{
		int o = objenum.GetIndex();
		int face_count = obj->GetFaceCount();

		for(int f = 0; f < face_count; f++)
		{
			// 無効な面(頂点の参照数が0)ならパス
			int ptcount = obj->GetFacePointCount(f);
			if(ptcount == 0) continue;

			obj->GetFacePointArray(f,indices);

			// 面が選択されている
			if(doc->IsSelectFace(o,f))
			{
				for(int p = 0; p < ptcount; p++)
					tmp.insert(MQSelectVertex(o,indices[p]));
				continue;
			}

			for(int p = 0; p < ptcount; p++)
			{
				if(doc->IsSelectLine(o,f,p))
				{
					tmp.insert(MQSelectVertex(o,indices[p]));
					tmp.insert(MQSelectVertex(o,indices[(p+1)%ptcount]));
				}
				if(doc->IsSelectVertex(o,indices[p]))
				{
					tmp.insert(MQSelectVertex(o,indices[p]));
				}
			}
		}
	}

	for(std::set<MQSelectVertex>::iterator it = tmp.begin(); it != tmp.end(); ++it)
	{
		out.push_back(*it);
	}
}	

static void get_symmetry_vertices(MQDocument doc, std::vector<MQSelectVertex>& in, std::vector<MQSelectVertex>& out)
{
	if(!s_editoption.Symmetry) return;

	//float distance = s_editoption.SymmetryDistance * s_editoption.SymmetryDistance;
	// なぜか0.0しか入ってこない。
	float distance = 1.0f;

	for(std::vector<MQSelectVertex>::iterator it = in.begin(); it != in.end(); ++it)
	{
		MQObject obj = doc->GetObject(it->object);
		if(obj == NULL) continue;

		MQPoint p0 = obj->GetVertex(it->vertex);
		p0.x = -p0.x;

		float mindist = distance;
		int symmetryv = -1;
		int vcount = obj->GetVertexCount();
		for(int i = 0; i < vcount; i++) 
		{
			if(obj->GetVertexRefCount(i) == 0) continue;
	
			MQPoint p1 = obj->GetVertex(i);

			float len = (p0-p1).norm();

			if(len > mindist) continue;

			mindist = len;
			symmetryv = i;
		}
		
		if(symmetryv != -1) out.push_back(MQSelectVertex(it->object,symmetryv));
	}
}

static bool is_point_in_triangle_2d(const MQPoint& p, const MQPoint& t1, const MQPoint& t2, const MQPoint& t3)
{
	return (
		((t2.x-t1.x) * (p.y-t1.y) - (t2.y-t1.y) * (p.x-t1.x) > 0) &&
		((t3.x-t2.x) * (p.y-t2.y) - (t3.y-t2.y) * (p.x-t2.x) > 0) &&
		((t1.x-t3.x) * (p.y-t3.y) - (t1.y-t3.y) * (p.x-t3.x) > 0) 
		);
}

static bool is_point_on_line_2d(const MQPoint& p, const MQPoint& t1, const MQPoint& t2)
{
	MQPoint v(t2 - t1); v.z = 0;
	float len = sqrtf(v.x * v.x + v.y * v.y);
	if(len == 0) return false;
	// normalize;
	v.x /= len;	v.y /= len;

	MQPoint roffset(-v.y * THRESHOLD_PICK_LINE, v.x * THRESHOLD_PICK_LINE, 0);

	v *= len * 2.0f;

	MQPoint triangle[3];

	triangle[0] = t1 + (roffset * 0.5f);
	triangle[1] = t1 + (roffset * -1.5f);
	triangle[2] = triangle[0] + v;

	if(!is_point_in_triangle_2d(p,triangle[0],triangle[1],triangle[2])) return false;

	triangle[0] = t2 + (roffset * -0.5f);
	triangle[1] = t2 + (roffset * 1.5f);
	triangle[2] = triangle[0] - v;

	if(!is_point_in_triangle_2d(p,triangle[0],triangle[1],triangle[2])) return false;

	return true;
}

static void get_vertex_disignated_normal(MQDocument doc, const MQSelectVertex& vaddr, MQPoint* nout)
{
	// 法線検出
	std::vector<MQPoint> normals;

	MQObject obj = doc->GetObject(vaddr.object);

	std::vector<int> faces;
	find_faces_contains_vertex(obj,vaddr.vertex,faces);

	for(std::vector<int>::iterator it = faces.begin(); it != faces.end(); ++it)
	{
		int vertices[4];
		int points = obj->GetFacePointCount(*it);
		obj->GetFacePointArray(*it,vertices);
		MQPoint n(FLT_MAX,0,0);
		if(points == 3) n = GetNormal(obj->GetVertex(vertices[0]),obj->GetVertex(vertices[1]),obj->GetVertex(vertices[2]));
		else if(points == 4) n = GetQuadNormal(obj->GetVertex(vertices[0]),obj->GetVertex(vertices[1]),obj->GetVertex(vertices[2]),obj->GetVertex(vertices[3]));
		else break;
		if(n.x == FLT_MAX) break;
		n.normalize();
		normals.push_back(n);
	}

	if(normals.size() == 0) { nout->zero(); return; }

	MQPoint& basenormal = normals[0];
	size_t samplesize = normals.size();

	// 両面を想定したnormalのスキップ
	for(std::vector<MQPoint>::iterator it = normals.begin(); it != normals.end(); ++it)
	{
		// 真逆を探す
		for(std::vector<MQPoint>::iterator it2 = normals.begin(); it2 != normals.end(); ++it2)
		{
			if(it2->norm() == 0) continue;
			if(GetInnerProduct(*it,*it2) < -0.999f)
			{
				it2->zero();
				samplesize--;
			}
		}
	}


	MQPoint n(0,0,0);
	for(std::vector<MQPoint>::iterator it = normals.begin(); it != normals.end(); ++it)
	{
		n += *it;
	}
	n /= (float)samplesize;
	n.normalize();

	*nout = n;
}



///////////////////////////////////////////////////////////////////////////////////////////////////////////////


void ExMovePlugin::refresh_edge_cache(MQDocument doc)
{
	m_cache_edges.clear();

	ObjectEnumerator objenum(doc,OE_SKIPHIDDEN | OE_SKIPLOCKED);
	for(MQObject obj = NULL; (obj = objenum.next()) != NULL; )
	{
		std::set< std::pair<int,int> > tmp;

		std::vector< std::vector<int> >& otarget = m_cache_edges[objenum.GetIndex()];
		otarget.reserve(100);

		int fcount = obj->GetFaceCount();
		for(int f = 0; f < fcount; f++)
		{
			std::vector<int> edges;
			edges.reserve(4);

			int ecount = obj->GetFacePointCount(f);
			int indices[4];
			obj->GetFacePointArray(f,indices);
			for(int i = 0; i < ecount; i++)
			{
				std::pair<int,int> p(indices[i],indices[(i+1)%ecount]);
				if(p.first < p.second) std::swap(p.first,p.second);
				if(tmp.find(p) == tmp.end())
				{
					tmp.insert(p);
					edges.push_back(i);
				}
			}
			otarget.push_back(edges);
		}
	}
}

void ExMovePlugin::refresh_cache(MQDocument doc,MQScene scene)
{
	m_cache_editable_faces.clear();
	m_cache_editable_vertices.clear();


	ObjectEnumerator objenum(doc);
	for(MQObject obj = NULL; (obj = objenum.next()) != NULL;)
	{
		std::set<int> vtmp;
		std::vector<int>& faces = m_cache_editable_faces[objenum.GetIndex()];

		int fcount = obj->GetFaceCount();
		BOOL* avisibility = new BOOL[fcount];
		scene->GetVisibleFace(obj,avisibility);


		for(int f = 0; f < fcount; f++)
		{
			if(IsFrontFace(scene,obj,f) && avisibility[f] == TRUE)
			{
				faces.push_back(f);
				int vindices[5] = {-1,-1,-1,-1,-1};
				obj->GetFacePointArray(f,vindices);
				for(int i = 0; vindices[i] != -1; i++) vtmp.insert(vindices[i]);
			}
		}
		std::vector<int>& vertices = m_cache_editable_vertices[objenum.GetIndex()];
		vertices.reserve(vtmp.size());
		for(std::set<int>::iterator it = vtmp.begin(); it != vtmp.end(); ++it) vertices.push_back(*it);
	}


}

void ExMovePlugin::pick_target(MQDocument doc, MQScene scene, POINT& mousepos, MQSelectElement* elm)
{
	elm->Reset();

	MQPoint clickpos((float)mousepos.x, (float)mousepos.y, 0);
	float mindist = THRESHOLD_PICK_POINT * THRESHOLD_PICK_POINT;

	MQSelectElement picked_item;

	MQSelectElement picked_vertex;
	int picked_edge[3] = {-1,-1,-1};

	float picked_item_z = 1.0f;

	if(s_editoption.EditVertex)
	{
		// pick a vertex
		ObjectEnumerator objenum(doc);
		float camera_z = 1.0f;
		for(MQObject obj = NULL; (obj = objenum.next()) != NULL;)
		{
			std::vector<int>& vertices = m_cache_editable_vertices[objenum.GetIndex()];

			for(std::vector<int>::iterator it = vertices.begin(); it != vertices.end(); ++it)
			{
				MQPoint sp = scene->Convert3DToScreen(obj->GetVertex(*it));
				if(sp.z < 0) continue;
				float dis2 = (sp.x-clickpos.x)*(sp.x-clickpos.x) + (sp.y-clickpos.y)*(sp.y-clickpos.y);
				if(mindist < dis2) continue;

				mindist = dis2;
				picked_vertex.SetVertex(objenum.GetIndex(),*it);
				camera_z = sp.z;
			}
		}
		if(!picked_vertex.IsEmpty()) 
		{
			picked_item = picked_vertex;
			picked_item_z = camera_z;
		}
	}

	if(s_editoption.EditFace || s_editoption.EditLine)
	{
		// pick faces and lines
		ObjectEnumerator objenum(doc);
		for(MQObject obj = NULL; (obj = objenum.next()) != NULL;)
		{
			int o = objenum.GetIndex();
			std::vector<int>& faces = m_cache_editable_faces[o];
			std::vector< std::vector<int> >& edges = m_cache_edges[o];
			
			for(std::vector<int>::iterator it = faces.begin(); it != faces.end(); ++it)
			{
				MQPoint t[4];
				int vindices[4];
				int pcount = obj->GetFacePointCount(*it);
				obj->GetFacePointArray(*it,vindices);
				t[0] = scene->Convert3DToScreen(obj->GetVertex(vindices[0]));
				t[1] = scene->Convert3DToScreen(obj->GetVertex(vindices[1]));
				if(pcount >= 3) t[2] = scene->Convert3DToScreen(obj->GetVertex(vindices[2]));
				if(pcount == 4) t[3] = scene->Convert3DToScreen(obj->GetVertex(vindices[3]));

				float z;

				// 自分の面を成す点が選択候補にある
				if(picked_vertex.GetObjectIndex() == o)
				{
					for(int p = 0; p < pcount; p++) if(picked_vertex.GetVertexIndex() == vindices[p]) { pcount = 0;/* to continue outer loop */ break; }
				}

				 // 自分の辺を成す点を含む辺が選ばれている
				if(picked_edge[0] == o) 
				{
					for(int p = 0; p < pcount; p++) if(picked_edge[1] == vindices[p] || picked_edge[2] == vindices[p]) { pcount = 0;/* do continue */ break; }
				}

				if(pcount == 0) continue;

				// lines
				if(s_editoption.EditLine)
				{
					if(*it >= (int)edges.size()) {debuglog(doc,"edge cache error %d %d %d", o, *it, edges.size());continue;}
					bool docontinue = false;
					std::vector<int>& e = edges[*it];
					for(std::vector<int>::iterator eit = e.begin(); eit != e.end(); ++eit)
					{
						int v0 = *eit;
						int v1 = (*eit+1)%pcount;
						if(is_point_on_line_2d(clickpos,t[v0],t[v1])) 
						{
							z = min(t[v0].z, t[v1].z); 
							if(z < picked_item_z)
							{
								picked_item.SetLine(o,*it,v0); 
								picked_item_z = z;
							
								picked_edge[0] = o;
								picked_edge[1] = v0;
								picked_edge[2] = v1;
							}
							docontinue = true;
							break;
						}
					}

					if(docontinue) continue;
				}

				if(pcount < 3) continue;

				// faces
				if(s_editoption.EditFace)
				{
					if(is_point_in_triangle_2d(clickpos,t[0],t[1],t[2])) 
					{
						z = min(min(t[0].z,t[1].z),t[2].z); 
						if(z < picked_item_z)
						{
							picked_item.SetFace(o,*it); 
							picked_item_z = z;
						}
						continue; 
					}
					if(pcount == 4)
					{
						if(is_point_in_triangle_2d(clickpos,t[0],t[2],t[3]))
						{
							z = min(min(t[0].z,t[2].z),t[3].z); 
							if(z < picked_item_z)
							{
								picked_item.SetFace(o,*it); 
								picked_item_z = z;
							}
							continue; 
						}
					}
				}
			}
		}
	}

	// nothing picked
	if(picked_item.IsEmpty()) return;

	// select least z
	*elm = picked_item;

}


//---------------------------------------------------------------------------
//  ExMovePlugin::Activate
//    表示・非表示切り替え要求
//---------------------------------------------------------------------------
BOOL ExMovePlugin::Activate(MQDocument doc, BOOL flag)
{
	m_highlightedelement.Reset();
	m_moved = false;

	if(flag == TRUE)
	{
		this->GetEditOption(s_editoption);
		m_cache_last_camera_pos.x = FLT_MAX;
		refresh_edge_cache(doc);

		char path[MAX_PATH];
		if(MQ_GetSystemPath(path, MQFOLDER_METASEQ_INI))
		{
			MQSetting set(path, "Screen");
			unsigned int col;
			set.Load("ColorHighlight",col,(unsigned int)63743);
			m_color_highlight.b = (float)((col >> 16) & 0xff) / 255.0f;
			m_color_highlight.g = (float)((col >> 8) & 0xff) / 255.0f;
			m_color_highlight.r = (float)(col & 0xff) / 255.0f;
		}

	}
	else
	{
		RedrawAllScene();
	}
	
	// そのままflagを返す
	return flag;
}



//---------------------------------------------------------------------------
//  ExMovePlugin::OnMouseMove
//    マウスが移動したとき
//---------------------------------------------------------------------------
BOOL ExMovePlugin::OnMouseMove(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state)
{
	this->GetEditOption(s_editoption);

	// カメラが移動していたら refresh_cache 一般的に適用可能なアイデアではない
	if(m_cache_last_camera_pos != scene->GetCameraPosition())	
	{
		refresh_cache(doc,scene);
		m_cache_last_camera_pos = scene->GetCameraPosition();
	}

	MQSelectElement elmnew;
	pick_target(doc,scene,state.MousePos,&elmnew);

	// 前回と違う頂点上にカーソルがきた場合は再描画
	if(m_highlightedelement != elmnew)
	{
		m_highlightedelement = elmnew;
		RedrawScene(scene);
	}

	// 独自処理を行ったが、標準動作も行わせるためFALSEを返す
	return FALSE;
}

//---------------------------------------------------------------------------
//  ExMovePlugin::OnDraw
//    描画時の処理
//---------------------------------------------------------------------------
void ExMovePlugin::OnDraw(MQDocument doc, MQScene scene, int width, int height)
{
	// 実はアクティブじゃなくても呼ばれてるので注意

	if(m_highlightedelement.IsEmpty()) return;

	MQObject obj = doc->GetObject(m_highlightedelement.GetObjectIndex());
	if (obj == NULL) return; // あり得ないはずだが、念のためNULLチェック

	// 頂点をハイライト描画
	switch(m_highlightedelement.GetType())
	{
	case SELEL_VERTEX:
	{
		// 頂点位置を計算
		MQPoint sp = scene->Convert3DToScreen(obj->GetVertex(m_highlightedelement.GetVertexIndex()));
		sp.z = 0.00001f;
		MQPoint newvp = scene->ConvertScreenTo3D(sp); 

		// 描画オブジェクトを生成
		int vertex[1];
		MQObject draw = CreateDrawingObject(doc, DRAW_OBJECT_POINT);
		draw->SetColor(m_color_highlight);
		draw->SetColorValid(TRUE);
		vertex[0] = draw->AddVertex(newvp);
		draw->AddFace(1, vertex);
	}
	break;

	case SELEL_LINE:
	{
		int indices[5];
		int pcount = obj->GetFacePointCount(m_highlightedelement.GetFaceIndex());
		obj->GetFacePointArray(m_highlightedelement.GetFaceIndex(),indices);

		int dindices[2];
		MQObject dobj = CreateDrawingObject(doc, DRAW_OBJECT_LINE);

		MQPoint ptmp = scene->Convert3DToScreen(obj->GetVertex(indices[m_highlightedelement.GetLineIndex()])); ptmp.z = 0.00001f;
		dindices[0] = dobj->AddVertex(scene->ConvertScreenTo3D(ptmp));
		ptmp = scene->Convert3DToScreen(obj->GetVertex(indices[(m_highlightedelement.GetLineIndex()+1)%pcount])); ptmp.z = 0.00001f;
		dindices[1] = dobj->AddVertex(scene->ConvertScreenTo3D(ptmp));

		dobj->AddFace(2,dindices);
		dobj->SetColor(m_color_highlight);
		dobj->SetColorValid(TRUE);
	}
	break;

	case SELEL_FACE:
	{
		int indices[5];
		int pcount = obj->GetFacePointCount(m_highlightedelement.GetFaceIndex());
		obj->GetFacePointArray(m_highlightedelement.GetFaceIndex(),indices);

		int dindices[5];
		MQObject dobj = CreateDrawingObject(doc, DRAW_OBJECT_FACE);

		for(int i = 0; i < pcount; i++)
		{
			MQPoint ptmp = scene->Convert3DToScreen(obj->GetVertex(indices[i]));
			ptmp.z = 0.00001f;
			dindices[i] = dobj->AddVertex(scene->ConvertScreenTo3D(ptmp));
		}
		dobj->AddFace(pcount,dindices);
		int dmatindex;
		MQMaterial dmat = CreateDrawingMaterial(doc,dmatindex);
		dmat->SetAlpha(0.5f);
		dmat->SetAmbient(0);
		dmat->SetDiffuse(0);
		dmat->SetEmission(1);
		dmat->SetPower(0);
		dmat->SetSpecular(0);
		dmat->SetShader(MQMATERIAL_SHADER_CLASSIC);
		dmat->SetColor(m_color_highlight);
		dobj->SetFaceMaterial(0,dmatindex);
	}
	break;
	}
}




//---------------------------------------------------------------------------
//  ExMovePlugin::OnLeftButtonDown
//    左ボタンが押されたとき
//---------------------------------------------------------------------------
BOOL ExMovePlugin::OnLeftButtonDown(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state)
{
	m_regional_select_mode = false;
	m_moved = false;
	m_normalmap.clear();

	MQSelectElement elm;
	pick_target(doc,scene,state.MousePos,&elm);

	m_togglereserve.Reset();
	
	if(elm.IsEmpty()) 
	{// 何もクリックしなかった
		m_regional_select_mode = true;
	}
	else
	{// 何かクリックした
		if(elm.IsSelected(doc))
		{// セレクション内
			m_togglereserve = elm;
		}
		else
		{// セレクション外
			if(state.Shift == FALSE)
			{
				doc->ClearSelect(MQDOC_CLEARSELECT_ALL);
			}
			elm.Select(doc);
		}
	}


	RedrawScene(scene);

	m_selection.clear();
	m_symmetry.clear();
	get_selection(doc,scene,m_selection);
	get_symmetry_vertices(doc,m_selection,m_symmetry);
	
	MQPoint p = elm.GetPoint(doc);
	m_sc_dragbegin_z = scene->Convert3DToScreen(p).z;
	m_mouse_sc_drag_x = state.MousePos.x;
	m_mouse_drag = scene->ConvertScreenTo3D(MQPoint((float)state.MousePos.x,(float)state.MousePos.y,m_sc_dragbegin_z));	
	m_mouse_drag_ignorey = scene->ConvertScreenTo3D(MQPoint((float)state.MousePos.x,0,m_sc_dragbegin_z));

	m_mouse_sc_dragbegin = MQPoint((float)state.MousePos.x,(float)state.MousePos.y,0.0001f);

	return TRUE;
}

//---------------------------------------------------------------------------
//  ExMovePlugin::OnLeftButtonMove
//    左ボタンが押されながらマウスが移動したとき
//---------------------------------------------------------------------------
BOOL ExMovePlugin::OnLeftButtonMove(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state)
{
	// region 選択モード
	if(m_regional_select_mode)
	{
		MQPoint mousepos((float)state.MousePos.x,(float)state.MousePos.y,0.0001f);

		int indices[8];
		MQObject dobj = CreateDrawingObject(doc, DRAW_OBJECT_LINE);

		MQPoint p(mousepos);
		indices[0] = dobj->AddVertex(scene->ConvertScreenTo3D(p));
		p.x = m_mouse_sc_dragbegin.x;
		indices[1] = dobj->AddVertex(scene->ConvertScreenTo3D(p));
		indices[2] = indices[0];
		p.x = mousepos.x;
		p.y = m_mouse_sc_dragbegin.y;
		indices[3] = dobj->AddVertex(scene->ConvertScreenTo3D(p));

		p = m_mouse_sc_dragbegin;
		indices[4] = dobj->AddVertex(scene->ConvertScreenTo3D(p));
		p.x = mousepos.x;
		indices[5] = dobj->AddVertex(scene->ConvertScreenTo3D(p));
		indices[6] = indices[4];
		p.x = m_mouse_sc_dragbegin.x;
		p.y = mousepos.y;
		indices[7] = dobj->AddVertex(scene->ConvertScreenTo3D(p));

		dobj->AddFace(2,&indices[0]);
		dobj->AddFace(2,&indices[2]);
		dobj->AddFace(2,&indices[4]);
		dobj->AddFace(2,&indices[6]);

		dobj->SetColor(MQColor(1,1,1));
		dobj->SetColorValid(TRUE);
		
		RedrawScene(scene);
		return TRUE;
	}

	// 頂点等ドラッグモード

	if(m_selection.size() == 0) return TRUE;

	// 移動済みチェック
	m_moved = true;

	// 頂点マージ
	if(state.RButton)
	{
		if(marge_vertices(doc,scene) == TRUE)
		{
			m_selection.clear();
			m_highlightedelement.Reset();
			RedrawAllScene();
			return TRUE;
		}
	}

	// 単なる移動
	if(state.Alt == TRUE)
	{
		MQPoint current_scene_mouse = scene->ConvertScreenTo3D(MQPoint((float)state.MousePos.x, (float)state.MousePos.y, m_sc_dragbegin_z));
		MQPoint delta  = (current_scene_mouse - m_mouse_drag);
	
		for(std::vector<MQSelectVertex>::iterator it = m_selection.begin(); it != m_selection.end(); ++it)
		{
			MQObject obj = doc->GetObject(it->object);
			MQPoint ipos = obj->GetVertex(it->vertex);
			obj->SetVertex(it->vertex, ipos + delta);
		}

		delta.x = -delta.x;
		for(std::vector<MQSelectVertex>::iterator it = m_symmetry.begin(); it != m_symmetry.end(); ++it)
		{
			MQObject obj = doc->GetObject(it->object);
			MQPoint ipos = obj->GetVertex(it->vertex);
			obj->SetVertex(it->vertex, ipos + delta);
		}

		m_mouse_drag = current_scene_mouse;

		RedrawAllScene();

		return TRUE;
	}

	// 法線移動 ////////////////////////////////

	// 初期処理
	if(m_normalmap.size() == 0)
	{
		// normalmap 作成
		for(std::vector<MQSelectVertex>::iterator it = m_selection.begin(); it != m_selection.end(); ++it)
		{
			MQPoint n(0,0,0);
			get_vertex_disignated_normal(doc,*it,&n);
			m_normalmap[*it] = n;
		}
		for(std::vector<MQSelectVertex>::iterator it = m_symmetry.begin(); it != m_symmetry.end(); ++it)
		{
			MQPoint n(0,0,0);
			get_vertex_disignated_normal(doc,*it,&n);
			m_normalmap[*it] = n;
		}
	}

	MQPoint current_scene_mouse = scene->ConvertScreenTo3D(MQPoint((float)state.MousePos.x,0,m_sc_dragbegin_z));	
	float dist = (current_scene_mouse - m_mouse_drag_ignorey).abs();
	if(state.MousePos.x - m_mouse_sc_drag_x < 0) dist = -dist;
	
	for(std::vector<MQSelectVertex>::iterator it = m_selection.begin(); it != m_selection.end(); ++it)
	{
		MQObject obj = doc->GetObject(it->object);
		MQPoint ipos = obj->GetVertex(it->vertex);
		obj->SetVertex(it->vertex, ipos + (m_normalmap[*it] * dist));
	}
	for(std::vector<MQSelectVertex>::iterator it = m_symmetry.begin(); it != m_symmetry.end(); ++it)
	{
		MQObject obj = doc->GetObject(it->object);
		MQPoint ipos = obj->GetVertex(it->vertex);
		obj->SetVertex(it->vertex, ipos + (m_normalmap[*it] * dist));
	}

	m_mouse_sc_drag_x = state.MousePos.x;
	m_mouse_drag_ignorey = current_scene_mouse;

	RedrawAllScene();

	return TRUE;
}


void ExMovePlugin::regional_select(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state)
{
	if(state.Shift == FALSE) doc->ClearSelect(MQDOC_CLEARSELECT_ALL);

	float r,l,b,t;

	r = max(m_mouse_sc_dragbegin.x,state.MousePos.x);
	l = min(m_mouse_sc_dragbegin.x,state.MousePos.x);
	t = max(m_mouse_sc_dragbegin.y,state.MousePos.y);
	b = min(m_mouse_sc_dragbegin.y,state.MousePos.y);

	ObjectEnumerator objenum(doc);
	for(MQObject obj = NULL; (obj = objenum.next()) != NULL;)
	{
		int vcount = obj->GetVertexCount();
		std::set<int> vertex_to_select;
		for(int v = 0; v < vcount; v++)
		{
			if(obj->GetVertexRefCount(v) == 0) continue;

			MQPoint p = scene->Convert3DToScreen(obj->GetVertex(v));
			if(r < p.x || l > p.x) continue;
			if(t < p.y || b > p.y) continue;

			doc->AddSelectVertex(objenum.GetIndex(),v);
			vertex_to_select.insert(v);
		}

		if(s_editoption.EditFace || s_editoption.EditLine)
		{
			// 両方が選択点の辺をさがす
			std::vector< std::vector<int> >& edges = m_cache_edges[objenum.GetIndex()];

			int fcount = obj->GetFaceCount();
			for(int f = 0; f < fcount; f++)
			{
				int indices[4];
				obj->GetFacePointArray(f,indices);
				int pcount = obj->GetFacePointCount(f);
				std::vector<int>& e = edges[f];
				for(std::vector<int>::iterator it = e.begin(); it != e.end(); ++it)
				{
					if(vertex_to_select.find(indices[*it]) != vertex_to_select.end() &&
						vertex_to_select.find(indices[(*it+1)%pcount]) != vertex_to_select.end())
					{
						doc->AddSelectLine(objenum.GetIndex(),f,*it);
					}
				}
			}
		}
	}
}

//---------------------------------------------------------------------------
//  ExMovePlugin::OnLeftButtonUp
//    左ボタンが離されたとき
//---------------------------------------------------------------------------
BOOL ExMovePlugin::OnLeftButtonUp(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state)
{
	//領域選択モード
	if(m_regional_select_mode)
	{
		regional_select(doc,scene,state);
		RedrawAllScene();
		return TRUE;
	}

	if(m_moved)
	{
		// 頂点が移動された場合、再描画とアンドゥの更新を行う
		RedrawAllScene();
		UpdateUndo();
	}
	else
	{
		// 動かさなかったときに選択済みをクリックしていたらトグルする
		if(!m_togglereserve.IsEmpty())
		{ 
			if(m_togglereserve.IsSelected(doc) && state.Shift)
			{
				m_togglereserve.Deselect(doc);
			}
		}
	}


	// 標準動作の代わりに独自処理を行ったのでTRUEを返す
	return TRUE;
}

BOOL ExMovePlugin::OnRightButtonDown(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state)
{
	return FALSE;
}

BOOL ExMovePlugin::marge_vertices(MQDocument doc, MQScene scene)
{
	if(m_selection.size() != 1) return FALSE;

	MQSelectVertex sv = *m_selection.begin();
	MQObject obj = doc->GetObject(sv.object);
	if(obj == NULL) return FALSE;

	MQPoint pbase(scene->Convert3DToScreen(obj->GetVertex(sv.vertex)));
	pbase.z = 0;

	// search neighbor
	int vcount = obj->GetVertexCount();
	float minlen = 15.0f * 15.0f;
	int vneighbor = -1;
	for(int v = 0; v < vcount; v++)
	{
		if(v == sv.vertex) continue;
		if(obj->GetVertexRefCount(v) == 0) continue;
		MQPoint p(scene->Convert3DToScreen(obj->GetVertex(v)));
		p.z = 0;
		float len = (p - pbase).norm();
		if(len < minlen)
		{
			vneighbor = v;
			minlen = len;
		}
	}

	if(vneighbor == -1) return TRUE;

	std::vector<int> findices;
	find_faces_contains_vertex(obj,sv.vertex,findices);

	for(std::vector<int>::iterator it = findices.begin(); it != findices.end(); ++it)
	{
		int indices[5];
		int newindices[5];
		int pcount = obj->GetFacePointCount(*it);
		obj->GetFacePointArray(*it,indices);
		int mat = obj->GetFaceMaterial(*it);
		obj->DeleteFace(*it,false);
		int newi = 0;
		for(int i = 0; i < pcount; i++) if(indices[i] != vneighbor) { newindices[newi] = indices[i]; newi++; }
		if(newi < 3) continue;
		for(int i = 0; i < newi; i++) if(newindices[i] == sv.vertex) newindices[i] = vneighbor;
		int newf = obj->AddFace(newi,newindices);
		obj->SetFaceMaterial(newf,mat);
	}

	m_moved = true;

	return TRUE;
}


//---------------------------------------------------------------------------
//  GetPluginClass
//    プラグインのベースクラスを返す
//---------------------------------------------------------------------------
MQBasePlugin *GetPluginClass()
{
	return &s_plugin;
}



//---------------------------------------------------------------------------
//  DllMain
//---------------------------------------------------------------------------
BOOL APIENTRY DllMain( HANDLE hModule, 
                       DWORD  ul_reason_for_call, 
                       LPVOID lpReserved
					 )
{
    return TRUE;
}


static void debuglog(MQDocument doc, const char* fmt, ...)
{
	va_list args;
	va_start(args,fmt);
	char buf[512];
	vsnprintf_s(buf,512,100,fmt,args);
	va_end(args);

	s_plugin.SendUserMessage(doc, 0x56A31D20, 0x9CE001E3, "ExMove", buf);
}
