/*
 *  Stage : a multi-robot simulator.
 *  Copyright (C) 2001, 2002 Richard Vaughan, Andrew Howard and Brian Gerkey.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */
/*
 * Desc: Base class for every entity.
 * Author: Richard Vaughan, Andrew Howard
 * Date: 7 Dec 2000
 * CVS info: $Id: entity.cc,v 1.118 2003-09-05 20:58:44 rtv Exp $
 */
#if HAVE_CONFIG_H
  #include <config.h>
#endif

//#define DEBUG
//#define VERBOSE
//#undef DEBUG
//#undef VERBOSE

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>
#include <float.h> // for FLT_MAX
#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/file.h>

#include <iostream>

#include "world.hh" 
#include "entity.hh"
#include "raytrace.hh"
#include "stage.h"

extern GHashTable* global_model_table;
extern int global_next_available_id;

// static method
gboolean CEntity::stg_update_signal( gpointer ptr )
{
  //PRINT_MSG1("static update called for %p", ptr );
  return ((CEntity*)ptr)->Update();
}


// plain functions for tree manipulation
CEntity* stg_ent_next_sibling( CEntity* ent )
{
  g_assert( ent->node );   
  g_assert( ent->node->data );   
  GNode* sib_node = g_node_next_sibling( ent->node );
  return((sib_node && sib_node->data)?(CEntity*)sib_node->data:NULL);
}

CEntity* stg_ent_first_child( CEntity* ent )
{ 
  g_assert( ent->node );   
  g_assert( ent->node->data );   
  GNode* child_node = g_node_first_child( ent->node );
  return((child_node && child_node->data)?(CEntity*)child_node->data:NULL);
}

CEntity* stg_world_first_child( stg_world_t* world )
{
  g_assert( world->node );   
  g_assert( world->node->data );   
  GNode* child_node = g_node_first_child( world->node );
  return((child_node && child_node->data )?(CEntity*)child_node->data:NULL);
}

// the root node of the tree is a stg_world_t
stg_world_t* stg_world( CEntity* ent )
{
  g_assert( ent->node );   
  g_assert( ent->node->data );   
  return( (stg_world_t*)g_node_get_root( ent->node )->data );
}

CEntity* stg_ent_parent( CEntity* ent )
{ 
  g_assert( ent->node );       
  g_assert( ent->node->data );   
  
  // if we have a parent node and it's not a root node (world)
  if( ent->node->parent && 
      (ent->node->parent->data != g_node_get_root(ent->node)->data) )
    return( (CEntity*)ent->node->parent->data );
  else
    return NULL;
}


void StgPrintTree( GNode* node, gpointer _prefix = NULL );

/* todo ?
typedef struct
{
  //stg_prop_id_t type;
  //const char* name;
  void* data;
  size_t len;
} stg_property_entry_t;
*/


///////////////////////////////////////////////////////////////////////////
// main constructor
CEntity::CEntity( stg_entity_create_t* init, stg_id_t id )
{
  // must set the name and token before using the ENT_DEBUG macro
  g_assert( init );
  this->id = id; // a unique id for this object
  this->name = g_string_new( init->name );
  
  ENT_DEBUG1( "entity construction - parent id: %d", init->parent_id );
  
  this->running = FALSE;

  // look up the parent id to find my parent's tree node
  GNode* parent_node = NULL;
  g_assert( (parent_node = (GNode*)g_hash_table_lookup( global_model_table, 
							&init->parent_id )));
  
  // add myself to the object tree
  g_assert( (this->node = g_node_append_data( parent_node, this )));      
  
#ifdef DEBUG
  // inspect the stg_world_t object at the root of the tree I just attached to. 
  stg_world_t* world = stg_world( this );
  g_assert( world );
  ENT_DEBUG2( "is in world %d:%s", world->id, world->name->str );
#endif
    
  // set up reasonble laser defaults
  this->laser_data.angle_min = -M_PI/2.0;
  this->laser_data.angle_max =  M_PI/2.0;
  this->laser_data.sample_count = STG_LASER_SAMPLES_MAX;
  this->laser_data.range_min = 0.1;
  this->laser_data.range_max = 8.0;
  memset( &this->laser_data.samples, 0,
	  STG_LASER_SAMPLES_MAX * sizeof(stg_laser_sample_t) );
  
  // reasonable neighbor detection bounds
  this->bounds_neighbor.min = 0.00;
  this->bounds_neighbor.max = 1.5 ;
  
  
  // TODO? = inherit our parent's color by default?
  if( strlen( init->color ) > 0 )
    this->color = stg_lookup_color(init->color); 
  else
    this->color = stg_lookup_color(STG_GENERIC_COLOR); 

  // we DO render in the matrix by default
  this->matrix_render = 1;

  // an empty array in which to store messages recieved from our neighbors
  this->received_msgs = g_array_new(FALSE,TRUE,sizeof(stg_los_msg_t) );
  
  // zero pose
  memset( &pose_local, 0, sizeof(pose_local) );
  
  // zero velocity
  memset( &velocity, 0, sizeof(velocity) );
  
  // zero origin
  memset( &pose_origin, 0, sizeof(pose_origin) );
  
  // zero last-mapped pose
  // zero pose
  memset( &pose_map, 0, sizeof(pose_map) );
  
  // unmoveably MASSIVE! by default
  this->mass = 1000.0;

  // default no-voltage.
  this->volts = -1;    

  // STG_PROP_POWER
  this->power_on = 1;

  // Set the default geometry
  this->size.x = this->size.y = 1.0;

  // initialize our shapes 
  this->rect_array = NULL;
    
  // the default entity is a colored box
  vision_return = true; 
  laser_return = LaserVisible;
  sonar_return = true;
  obstacle_return = true;
  idar_return = IDARReflect;
  puck_return = false;
  idar_return = IDARReflect;
  fiducial_return = FiducialNone; // not a recognized fiducial
  gripper_return = GripperDisabled;
  neighbor_return = 0;

  // a null array of range sensors
  this->rangers = NULL;
  
  // indicator light off
  memset( &this->blinkenlight, 0, sizeof(this->blinkenlight) );
  
  //m_dependent_attached = false;
  
  this->mouseable = true;
  this->draw_nose = true;
  this->interval = 0.01; // default update interval in seconds 
  this->border = false;

  // STG_PROP_RANGEBOUNDS
  this->min_range = 0.5;
  this->max_range = 5.0;

  
#ifdef DEBUG
  CEntity* parent = stg_ent_parent( this );
  if( parent ) ENT_DEBUG2( "has parent %d:%s",  parent->id, parent->name->str );
#endif  

  // gui data will be attached here
  this->guimod = NULL;

  ENT_DEBUG("entity construction complete");

  // must do this before setting the default rectangle, or we'll never see it.
  this->guimod = stg_gui_model_create( this );
  
  ENT_DEBUG( "entity startup" );
  
  // by default, all entities have a single rectangle that is
  // automagically scaled to fit the size of the entity
  stg_rotrect_t rect;
  rect.x = 0.0;
  rect.y = 0.0;
  rect.a = 0.0;
  rect.w = 1.0; // this unit is multiples of my body width, not a fixed size
  rect.h = 1.0; // ditto
    
  this->SetProperty( STG_PROP_RECTS, &rect, sizeof(rect) );

  Map(); // render in matrix

  /* the LAST THING WE DO is to request callbacks into this object */
  
  // todo - real time vs. non-real time
  if( this->interval > 0 )
    {
      guint ms_interval = (guint)(this->interval * 1000.0);
      update_tag = g_timeout_add(ms_interval,CEntity::stg_update_signal, this);
    } 

  // don't put anything here!
  
  ENT_DEBUG( "entity startup complete" );
}


///////////////////////////////////////////////////////////////////////////
// Destructor
CEntity::~CEntity()
{
  ENT_DEBUG("entity destruction");
  //if( this->running ) this->Shutdown(); 
  
  ENT_DEBUG( "entity shutdown" );
  
  UnMap();
  
  /* cancel the callbacks into this object */
  g_source_remove( update_tag );
  
  //ENT_DEBUG( "shutting down children" );
  
  //for( CEntity* child = stg_ent_first_child( this ); child; 
  //   child = stg_ent_next_sibling(child))
  // child->Shutdown();
  
  //ENT_DEBUG( "finished shutting down children" ); 
  
  this->running = FALSE;
  

  // deleting a child removes it from the tree, so we can't iterate
  // safely here; instead we repeatedly delete the first child until
  // they're all gone.
  CEntity* child = NULL;
  while( (child = stg_ent_first_child( this ) ))
    {
      ENT_DEBUG2( "entity destruction - destroying child [%d:%s]",
		   child->id, child->name->str );
      delete child;
      ENT_DEBUG( "entity destruction - destroying child complete" );
    }

  if( this->guimod) 
    {
      ENT_DEBUG( "shutting down GUI" );
      stg_gui_model_destroy( this->guimod );
      this->guimod = NULL;
    }
  
  //ENT_DEBUG( "entity shutdown complete" );

  
  g_array_free( this->received_msgs, TRUE );

  // detatch myself from my parent
  g_node_unlink( this->node );
  g_node_destroy( this->node );
  
  ENT_DEBUG("entity destruction complete");
  // actually, we still have to free up the strings that the previous
  // trace statement uses...
  if( name ) g_string_free( name, TRUE );
}

// returns the stg_world_t object at the bottom of my node tree
stg_world_t* CEntity::GetWorld()
{
  return((stg_world_t*)g_node_get_root(this->node)->data);
}

// returns the CMatrix object we are rendering into
CMatrix* CEntity::GetMatrix()
{
  return( this->GetWorld()->matrix );
}

void CEntity::GetBoundingBox( double &xmin, double &ymin,
			      double &xmax, double &ymax )
{
  double x[4];
  double y[4];

  double dx = this->size.x / 2.0;
  double dy = this->size.y / 2.0;
  double dummy = 0.0;
  
  x[0] = pose_origin.x + dx;
  y[0] = pose_origin.y + dy;
  this->LocalToGlobal( x[0], y[0], dummy );

  x[1] = pose_origin.x + dx;
  y[1] = pose_origin.y - dy;
  this->LocalToGlobal( x[1], y[1], dummy );

  x[2] = pose_origin.x - dx;
  y[2] = pose_origin.y + dy;
  this->LocalToGlobal( x[2], y[2], dummy );

  x[3] = pose_origin.x - dx;
  y[3] = pose_origin.y - dy;
  this->LocalToGlobal( x[3], y[3], dummy );

  //double ox, oy, oth;
  //GetGlobalPose( ox, oy, oth );
  //printf( "pose_origin: %.2f,%.2f,%.2f \n", ox, oy, oth );
  //printf( "corners: \n" );
  //for( int c=0; c<4; c++ )
  //printf( "\t%.2f,%.2f\n", x[c], y[c] );
  
  // find the smallest values and we're done
  //xmin = xmax = x[0];
  for( int c=0; c<4; c++ )
  {
    if( x[c] < xmin ) xmin = x[c];
    if( x[c] > xmax ) xmax = x[c];
  }

  //ymin = ymax = y[0];
  for( int c=0; c<4; c++ )
  {
    if( y[c] < ymin ) ymin = y[c];
    if( y[c] > ymax ) ymax = y[c];
  } 

  //printf( "before children: %.2f %.2f %.2f %.2f\n",
  //  xmin, ymin, xmax, ymax );

  //CHILDLOOP( ch )
  // ch->GetBoundingBox(xmin, ymin, xmax, ymax ); 
  
  //printf( "after children: %.2f %.2f %.2f %.2f\n",
  //  xmin, ymin, xmax, ymax );

}


int CEntity::Move( stg_velocity_t* vel, double step )
{
  //ENT_DEBUG("");
    
    //fprintf( stderr, "%.2f %.2f %.2f   %.2f %.2f %.2f\n", 
    //   px -1, py -1, pa, odo_px, odo_py, odo_pa );
    
  if( vel->x == 0 && vel->y == 0 && vel->a == 0 )
    return 0;
  
  // Compute movement deltas
  // This is a zero-th order approximation
  double cosa = cos(this->pose_local.a);
  double sina = sin(this->pose_local.a);

  double dx = step * vel->x * cosa - step * vel->y * sina;
  double dy = step * vel->x * sina + step * vel->y * cosa;
  double da = step * vel->a;
  
  // compute a new pose by shifting us a little from the current pose
  stg_pose_t newpose;
  newpose.x = pose_local.x + dx;
  newpose.y = pose_local.y + dy;
  newpose.a = pose_local.a + da;

  // Check for collisions
  // and accept the new pose if ok
  if( TestCollision() != NULL )
    {
      stg_velocity_t vel;
      memset( &vel, 0, sizeof(vel) );
      //SetVelocity( &vel );
      this->stall = true;
    }
  else
    SetProperty( STG_PROP_POSE, &newpose, sizeof(newpose) );  
  
  return 0; // success
}
///////////////////////////////////////////////////////////////////////////
// Update the entity's representation
gboolean CEntity::Update()
{
  // PRINT_DEBUG2("update called for %d:%s", this->id, this->name->str );
  //PRINT_DEBUG3( "vx %.2f vy %.2f vth %.2f", vx, vy, vth);

  Move( &this->velocity, this->interval );
  
  return TRUE;
}

///////////////////////////////////////////////////////////////////////////
// Render the entity into the world
void CEntity::Map( stg_pose_t* pose )
{  
  if( this->matrix_render == 0 )
    return;
  
  // we're figuring out the mapped pose from this new pose
  memcpy( &this->pose_map, pose, sizeof(stg_pose_t) );
  
  // get the pose in local coords
  this->GlobalToLocal( &this->pose_map );
  
  // add our center of rotation offsets
  this->pose_map.x += pose_origin.x;
  this->pose_map.y += pose_origin.y;

  // convert back to global coords
  this->LocalToGlobal( &this->pose_map );

  MapEx( &this->pose_map, true);
}

void CEntity::Map( void )
{ 
  if( this->matrix_render == 0 )
    return;
  
  stg_pose_t pose;
  GetGlobalPose( &pose );
  Map( &pose );
}

///////////////////////////////////////////////////////////////////////////
// Remove the entity from the world
void CEntity::UnMap()
{
  MapEx( &this->pose_map, false);
}


///////////////////////////////////////////////////////////////////////////
// Remap ourself if we have moved
void CEntity::ReMap( stg_pose_t* pose )
{
  stg_world_t* world = this->GetWorld();
  
  // if we have moved less than 1 pixel, do nothing
  if (fabs(pose->x - this->pose_map.x) < 1.0 / world->ppm &&
      fabs(pose->y - this->pose_map.y) < 1.0 / world->ppm &&
      pose->a == this->pose_map.a)
    return;
  
  // otherwise erase the old render and draw a new one
  UnMap();
  Map(pose);
}


///////////////////////////////////////////////////////////////////////////
// Primitive rendering function
void CEntity::MapEx( stg_pose_t* pose, bool render)
{
    RenderRects( render );
}

////////////////////////////////////////////////////////////////////////////
// convert the rotated rectangle into global coords, taking into account
// the entities pose and offset and the rectangle scaling
void CEntity::GetGlobalRect( stg_rotrect_t* dest, stg_rotrect_t* src )
{
  dest->x = ((src->x + src->w/2.0) * this->size.x) - this->size.x/2.0 + this->pose_origin.x;
  dest->y = ((src->y + src->h/2.0) * this->size.y) - this->size.y/2.0 + this->pose_origin.y;
  
  dest->a = src->a;
  dest->w = src->w * this->size.x;
  dest->h = src->h * this->size.y;
  
  LocalToGlobal( dest->x, dest->y, dest->a );    
}


///////////////////////////////////////////////////////////////////////////
// Check to see if the given pose will yield a collision with obstacles.
// Returns a pointer to the first entity we are in collision with, and stores
// the location of the hit in hitx,hity (if non-null)
// Returns NULL if not collisions.
// This function is useful for writing position devices.
CEntity *CEntity::TestCollision( double* hitx, double* hity )
{
  // raytrace along all our rectangles. expensive, but most vehicles
  // will just be a single rect, grippers 3 rects, etc. not too bad.
  
  stg_rotrect_t glob;
  
  // no rects? no collision
  if( this->rect_array == NULL )
    return NULL;

  if( this->rect_array->len == 0 )
    {
      PRINT_WARN( "no rectangles in rect array" );
      return NULL;
    }

  int r;
  for( r=0; r<(int)this->rect_array->len; r++ )
    {
      // find the global coords of this rectangle
      GetGlobalRect( &glob, 
		     &g_array_index(this->rect_array, stg_rotrect_t, r) );
      
      // trace this rectangle in the matrix
      CRectangleIterator rit( glob.x, glob.y, glob.a, 
			      glob.w, glob.h, this->GetMatrix() );
      
      CEntity* ent;
      while( (ent = rit.GetNextEntity()) )
	{
	  if( ent != this && !IsDescendent(ent) && ent->obstacle_return )
	    //if( ent != this && ent->obstacle_return )
	    {
	      if( hitx || hity ) // if the caller needs to know hit points
		{
		  double x, y;
		  rit.GetPos(x,y); // find the points		  
		  if( hitx ) *hitx = x; // and report them
		  if( hity ) *hity = y;

		}
	      return ent; // we hit this object! stop raytracing
	    }
	}
    }
  return NULL;  // done 
}
  
bool CEntity::IsDescendent( CEntity* ent )
{
  return( g_node_is_ancestor( this->node, ent->node ) );
}


void CEntity::LocalToGlobal( double &x, double &y, double &a )
{
  stg_pose_t pose;
  pose.x = x;
  pose.y = y;
  pose.a = a;

  this->LocalToGlobal( &pose );

  x = pose.x;
  y = pose.y;
  a = pose.a;
} 

///////////////////////////////////////////////////////////////////////////
// Convert local to global coords
void CEntity::LocalToGlobal( stg_pose_t* pose )
{
  // Get the pose of our origin wrt global cs
  stg_pose_t origin;
  GetGlobalPose( &origin );

  // Compute pose based on the parent's pose
  double cosa = cos(origin.a);
  double sina = sin(origin.a);
  double sx = origin.x + pose->x * cosa - pose->y * sina;
  double sy = origin.y + pose->x * sina + pose->y * cosa;
  double sth = origin.a + pose->a;
  
  pose->x = sx;
  pose->y = sy;
  pose->a = sth;
}


///////////////////////////////////////////////////////////////////////////
// Convert global to local coords
//
void CEntity::GlobalToLocal( stg_pose_t* pose )
{
  // Get the pose of our origin wrt global cs
  stg_pose_t origin;
  GetGlobalPose( &origin );

  // Compute pose based on the parent's pose
  double cosa = cos(origin.a);
  double sina = sin(origin.a);
  double sx =  (pose->x - origin.x) * cosa + (pose->y - origin.y) * sina;
  double sy = -(pose->x - origin.x) * sina + (pose->y - origin.y) * cosa;
  double sth = pose->a - origin.a;

  pose->x = sx;
  pose->y = sy;
  pose->a = sth;
}


int CEntity::SetProperty( stg_prop_id_t ptype, void* data, size_t len )
{
  ENT_DEBUG3( "Setting prop %s (%d) with %d bytes", 
	      stg_property_string(ptype), ptype, len );
  
  switch( ptype )
    {      
    case STG_PROP_MATRIX_RENDER:
      this->UnMap();
      g_assert( (len == sizeof(stg_matrix_render_t)) );	
      this->matrix_render = *(stg_matrix_render_t*)data;
      this->Map();
      break;
      
    case STG_PROP_POSE:
      g_assert( (len == sizeof(stg_pose_t)) );	
      this->SetPose( (stg_pose_t*)data );
      break;
      
    case STG_PROP_SIZE:
      g_assert( (len == sizeof(stg_size_t)) );	
      this->SetSize(  (stg_size_t*)data );
      break;
      
    case STG_PROP_ORIGIN:
      g_assert( (len == sizeof(stg_pose_t)) );	
      this->SetOrigin((stg_pose_t*)data );
      break;

    case STG_PROP_VELOCITY:
      g_assert( (len == sizeof(stg_velocity_t)) );
      this->SetVelocity( (stg_velocity_t*)data );
      break;
      
    case STG_PROP_NEIGHBORRETURN:
      g_assert( (len == sizeof(int)) );	
      this->neighbor_return = *(int*)data;
      break;
      
    case STG_PROP_LASERRETURN:
      g_assert( (len == sizeof(stg_laser_return_t)) );	
      this->laser_return = *(stg_laser_return_t*)data;
      break;

    case STG_PROP_BLINKENLIGHT:
      g_assert( (len == sizeof(stg_blinkenlight_t) ));	
      memcpy( &this->blinkenlight, data, sizeof(this->blinkenlight) );
      break;      
      
    case STG_PROP_NOSE:
      g_assert( (len == sizeof(stg_nose_t)) );	
      this->draw_nose = *(stg_nose_t*)data;
      break;      
      
    case STG_PROP_MOUSE_MODE:
      g_assert( (len == sizeof(stg_mouse_mode_t)) );	
      this->mouseable = *(stg_mouse_mode_t*)data;
      break;      

    case STG_PROP_BORDER:
      g_assert( (len == sizeof(stg_border_t)) );	
      this->border = *(stg_border_t*)data;
      break;      

    case STG_PROP_NEIGHBORBOUNDS:
      g_assert( (len == sizeof(stg_bounds_t)) );	
      memcpy( &this->bounds_neighbor, (stg_bounds_t*)data, sizeof(stg_bounds_t));
      break;
      
    case STG_PROP_LOS_MSG:
    case STG_PROP_LOS_MSG_CONSUME:
      g_assert( (len == sizeof(stg_los_msg_t)) );	
      this->SendLosMessage( (stg_los_msg_t*)data );
      break;

    case STG_PROP_RANGERS:
      {
	if( this->rangers ) g_array_free( this->rangers, TRUE );
	
	// we infer the number of rangers from the data size
	int tcount = len / sizeof(stg_ranger_t);
	
	/*for( int t=0; t<tcount; t++ )
	  {
	  stg_ranger_t* rgr = ((stg_ranger_t*)data) + t;
	  
	  printf( "setting ranger %d (%.2f,%.2f,%.2f)[%.2f %.2f]\n",
	  t, 
	  rgr->pose.x, rgr->pose.y, rgr->pose.a,
	  rgr->size.x, rgr->size.y );
	  } 
	*/

	/*
	// gtk+-2.0
	this->rangers = g_array_sized_new( FALSE, TRUE, 
	sizeof(stg_ranger_t), tcount );
	*/
	
	// gtk+1.2 or 2.0
	this->rangers = g_array_new( FALSE, TRUE, 
				       sizeof(stg_ranger_t));
	
	this->rangers = g_array_append_vals( this->rangers, data, tcount );
      }
      break;

    case STG_PROP_RECTS:
      {
	// we infer the number of rects from the data size
	int rect_count = len / sizeof(stg_rotrect_t);
	// squeeze all these rects into a unit square
	this->NormalizeRects( (stg_rotrect_t*)data, rect_count );
	// and accept them as my personal rectangles
	this->SetRects( (stg_rotrect_t*)data, rect_count );
      }
      break;
      
    default:
      PRINT_WARN1( "unhandled property type (%d)", ptype );
      break;
    }

  // let our GUI representation reflect the changes
  if( this->guimod ) stg_gui_model_update( this, ptype );
  
 return 0; // success
}

stg_property_t* CEntity::GetProperty( stg_prop_id_t ptype )
{    
  ENT_DEBUG2( "Getting prop %s (%d)", 
        stg_property_string(ptype), ptype );

  stg_property_t* prop = stg_property_create();
  prop->id = this->id;
  prop->property = ptype;
  prop->timestamp = 100.0;
  
  switch( ptype )
    { 
    case STG_PROP_MATRIX_RENDER:
      prop = stg_property_attach_data( prop, 
				       &this->matrix_render, 
				       sizeof(this->matrix_render) );
      break;
      
    case STG_PROP_POSE:
      stg_pose_t pose;
      this->GetPose(&pose);
      prop = stg_property_attach_data( prop, 
				       &pose, 
				       sizeof(pose) );
      break;
      
    case STG_PROP_SIZE:
      prop = stg_property_attach_data( prop, 
				       &this->size, 
				       sizeof(this->size) );
      break;
      
    case STG_PROP_ORIGIN:
      prop = stg_property_attach_data( prop, 
				       &this->pose_origin, 
				       sizeof(this->pose_origin) );
      break;

    case STG_PROP_VELOCITY:
      prop = stg_property_attach_data( prop, 
				       &this->velocity, 
				       sizeof(this->velocity));
      break;

    case STG_PROP_LASER_DATA:
      this->UpdateLaserData( &this->laser_data );
      prop = stg_property_attach_data( prop, 
				       &this->laser_data,
				       sizeof(stg_laser_data_t) );
      break;
      
    case STG_PROP_NEIGHBORRETURN:
      prop = stg_property_attach_data( prop, 
				       &this->neighbor_return, 
				       sizeof(this->neighbor_return));
      break;

    case STG_PROP_NEIGHBORBOUNDS:
      prop = stg_property_attach_data( prop, 
				       &this->bounds_neighbor, 
				       sizeof(this->bounds_neighbor));

    case STG_PROP_BLINKENLIGHT:
      prop = stg_property_attach_data( prop, 
				       &this->blinkenlight, 
				       sizeof(this->blinkenlight));
      break;

    case STG_PROP_NOSE:
      prop = stg_property_attach_data( prop, 
				       &this->draw_nose, 
				       sizeof(this->draw_nose));
      break;

    case STG_PROP_BORDER:
      prop = stg_property_attach_data( prop, 
				       &this->border, 
				       sizeof(this->border));
      break;

    case STG_PROP_LASERRETURN:
      prop = stg_property_attach_data( prop, 
				       &this->laser_return, 
				       sizeof(this->laser_return));
      break;
      
    case STG_PROP_LOS_MSG:
      if( this->received_msgs->len > 0 )
	{
	  // return just the FIRST ITEM in the message array
	  prop = stg_property_attach_data( prop, 
					   this->received_msgs->data,
					   sizeof(stg_los_msg_t) );
	}
      break;
 
    case STG_PROP_LOS_MSG_CONSUME:
      if( this->received_msgs->len > 0 )
	{
	  // return just the FIRST ITEM in the message array
	  prop = stg_property_attach_data( prop, 
					   this->received_msgs->data,
					   sizeof(stg_los_msg_t) );
	  // and consume the item
	  this->received_msgs = g_array_remove_index(this->received_msgs, 0);
	}
      break;
      
    case STG_PROP_MOUSE_MODE:
      prop = stg_property_attach_data( prop, 
				       &this->mouseable,
				       sizeof(this->mouseable));
      break;
      
    case STG_PROP_NEIGHBORS:
      {
	GArray* array = NULL;
	this->GetNeighbors( &array );
	
	prop = stg_property_attach_data( prop, 
					 array->data, 
					 array->len*sizeof(stg_neighbor_t));
	g_array_free( array, TRUE );
      }
      break;
      
    case STG_PROP_RANGERS:
      if( this->rangers ) 
	{
	  this->UpdateRangers();
	  
	  prop = stg_property_attach_data( prop, 
					   this->rangers->data,
					   this->rangers->len * 
					   sizeof(stg_ranger_t) );
	}
      else
	PRINT_WARN( "requested ranger data, but this model has"
		    " no rangers" ); 
      break;
      
    case STG_PROP_RECTS:
      if( this->rect_array != NULL )
	prop = stg_property_attach_data( prop, 
					 this->rect_array->data, 
					 this->rect_array->len * 
					 sizeof(stg_rotrect_t) );
      break;

      
    default:
      PRINT_WARN2( "unhandled property type %s (%d) - returning blank property", 
		   stg_property_string(ptype), ptype );
      break;
    }
  //stg_property_print( prop );
  return prop;
};

CEntity* StgEntityFromId( stg_id_t id )
{
  // get the entity with this id from the global hash table   
  GNode* node = 
    (GNode*)g_hash_table_lookup( global_model_table, &id );
  
  if( node == NULL ) return NULL;

  return (CEntity*)node->data;	
}

void CEntity::SendLosMessage( stg_los_msg_t* msg )
{
  // sanity checking
  g_assert( msg );
  g_assert( msg->len < STG_LOS_MSG_MAX_LEN );
  
  //stg_los_msg_print( msg );

  stg_gui_los_msg_send( this, msg );

  if( msg->id > 0 ) // we're targeting a specific entity
    {
      CEntity *target = StgEntityFromId( msg->id );
      
      if( target == NULL ) // no one to talk to
	return;

      if( !target->neighbor_return ) // not a neighbor!
      	return;
      
      // compute the range the message will travel
      stg_pose_t my_pose, target_pose;
      this->GetGlobalPose( &my_pose );
      target->GetGlobalPose( &target_pose );
      
      double dx = my_pose.x - target_pose.x;
      double dy = my_pose.y - target_pose.y;
      double range = sqrt( dx*dx + dy*dy);
      
      // Filter by detection range
      if( range > this->bounds_neighbor.max || 
	  range < this->bounds_neighbor.min )
	return;
      
      // as a last resort (because this is so expensive) filter by LOS
      if( OcclusionTest(target) )
	return;
      
      // ok the message gets through!
      stg_los_msg_t recv;
      memcpy( &recv, msg, sizeof(recv) );
      recv.id = this->id;
      target->received_msgs = 
	g_array_append_vals( target->received_msgs, &recv, 1 );
      
      //for( int i=0; i<(int)target->received_msgs->len; i++ )
      //stg_los_msg_print( &g_array_index( target->received_msgs, 
      //				   stg_los_msg_t, i ) );
      
      stg_gui_los_msg_recv( target, this, msg );
    }
  else
    {
      // Search through the global device list and send the message to
      // anything in range
      GNode* root_node = g_node_get_root(this->node);
      
      // for each node in my world's tree
      for( GNode* node = g_node_first_child(root_node);
	   node; 
	   node = g_node_next_sibling( node) )
	{
	  CEntity* target = (CEntity*)node->data;
	  
	  if( !target->neighbor_return ) // not a neighbor!
	    continue;
	  

	  // compute the range the message will travel
	  stg_pose_t my_pose, target_pose;
	  this->GetGlobalPose( &my_pose );
	  target->GetGlobalPose( &target_pose );
	  
	  double dx = my_pose.x - target_pose.x;
	  double dy = my_pose.y - target_pose.y;
	  double range = sqrt( dx*dx + dy*dy);
	  
	  // Filter by detection range
	  if( range > this->bounds_neighbor.max || 
	      range < this->bounds_neighbor.min )
	    continue;
	  
	  // as a last resort (because this is so expensive) filter by LOS
	  if( OcclusionTest(target) )
	    continue;
	  
	  // ok the message gets through! add it to the receiver's
	  // list, with our id poked in.
	  stg_los_msg_t recv;
	  memcpy( &recv, msg, sizeof(recv) );
	  recv.id = this->id;
	  g_array_append_vals( target->received_msgs, &recv, 1 );
	  
	  //printf( "target %d received message\n", msg->id ); 
	  //puts( "target's message array:" );
	  //for( int i=0; i<(int)target->received_msgs->len; i++ )
	  // stg_los_msg_print( &g_array_index( target->received_msgs, 
	  //			       stg_los_msg_t, i ) );
	  
	  stg_gui_los_msg_recv( target, this, msg );	  
	}
    }
}

void CEntity::GetNeighbors( GArray** neighbor_array )
{
  //PRINT_DEBUG( "searching for neighbors" );

  // create the array, pre-allocating some space for speed
  //*neighbor_array = g_array_sized_new( FALSE, TRUE, 
  //			       sizeof(stg_neighbor_t), 
  //			       10 ); 
  *neighbor_array = g_array_new( FALSE, TRUE, 
				 sizeof(stg_neighbor_t) );
  
  // Search through the global device list looking for devices that
  // have a non-zero neighbor-return
  GNode* root_node = g_node_get_root(this->node);

  // for each node in my world's tree
  for( GNode* node = g_node_first_child(root_node);
       node; 
       node = g_node_next_sibling( node) )
    {
      CEntity* ent = (CEntity*)node->data;

      if( !ent->neighbor_return ) // not a neighbor!
      	continue;

      stg_pose_t pz;
      ent->GetGlobalPose( &pz );

      stg_size_t sz;
      ent->GetSize( &sz );
      
      // Compute range and bearing of entity relative to sensor
      //
      double dx = pz.x - pose_local.x;
      double dy = pz.y - pose_local.y;

      // construct a neighbor packet
      stg_neighbor_t buddy;
      buddy.id = ent->id;
      buddy.range = sqrt(dx * dx + dy * dy);
      buddy.bearing = NORMALIZE(atan2(dy, dx) - pose_local.a);
      buddy.orientation = NORMALIZE(pz.a - pose_local.a);
      buddy.size.x = sz.x;
      buddy.size.y = sz.y;

      //PRINT_DEBUG3( "detected a neighbor id %d at %.2fm, %.2f degrees",
      //	    buddy.id, buddy.range, buddy.bearing );

      // Filter by detection range
      if( buddy.range > this->bounds_neighbor.max || 
	  buddy.range < this->bounds_neighbor.min )
	continue;
      
      // todo - Filter by view angle
      //if( fabs(bearing) > view_angle/2.0 )
      //continue;

      // as a last resort (because this is so expensice) filter by LOS
      //if( this->perform_occlusion_test && OcclusionTest(ent) )
      if( OcclusionTest(ent) )
	continue;
      
      *neighbor_array = g_array_append_vals( *neighbor_array, &buddy, 1 );
    }


  stg_gui_neighbor_render( this, *neighbor_array );
}

bool CEntity::OcclusionTest(CEntity* ent )
{
  // Compute parameters of scan line
  stg_pose_t start;
  stg_pose_t end;
  //double startx, starty, endx, endy, dummyth;
  this->GetGlobalPose( &start );
  ent->GetGlobalPose( &end );
  
  CLineIterator lit( start.x, start.y, end.x, end.y,
		     this->GetWorld()->matrix,  PointToPoint );
  CEntity* hit;
  
  while( (hit = lit.GetNextEntity()) )
    {
      if( hit == ent ) // if we hit the entity there was no occlusion
	return false;
      
      // we use laser visibility for basic occlusion
      //if( hit != this && hit != m_parent_entity && hit->laser_return ) 
      if( hit != this && hit->laser_return ) 
	return true;
    }	
  
  // we didn't hit the target and we ran out of entities (a little
  // wierd, but it's certainly not a hit...
  return true;  
}


///////////////////////////////////////////////////////////////////////////
// Set the entitys pose in the parent cs
void CEntity::SetPose( stg_pose_t* pose )
{
  //ENT_DEBUG3( "setting pose to [%.2f %.2f %.2f]", pose->x, pose->y, pose->a );  
  // if the new position is different, call SetProperty to make the change.
  if( memcmp( &this->pose_local, pose, sizeof( stg_pose_t)) != 0 )
    {
      this->UnMapWithDescendents();
      memcpy( &this->pose_local, pose, sizeof(stg_pose_t) );
      this->MapWithDescendents();
    }
}

///////////////////////////////////////////////////////////////////////////
// Set the entity's origin in the parent cs
void CEntity::SetOrigin( stg_pose_t* pose )
{
  //ENT_DEBUG3( "setting origin to [%.2f %.2f %.2f]", pose->x, pose->y, pose->a );
  // if the new position is different, call SetProperty to make the change.
  if( memcmp( &this->pose_origin, pose, sizeof( stg_pose_t)) != 0 )
    {
      this->UnMapWithDescendents();
      memcpy( &this->pose_origin, pose, sizeof(stg_pose_t) );
      this->MapWithDescendents();
    }
}

// unmap myself and all my children
void CEntity::UnMapWithDescendents( void )
{
  for( CEntity* child = stg_ent_first_child( this ); child; 
       child = stg_ent_next_sibling(child))
    child->UnMap();
  
  this->UnMap();
}

// map myself and all my children
void CEntity::MapWithDescendents( void )
{
  for( CEntity* child = stg_ent_first_child( this ); child; 
       child = stg_ent_next_sibling(child))
    child->Map();
  
  this->Map();
}

void CEntity::SetSize( stg_size_t* sz )
{
  // if a dimension is < 0, set it to be as large as the whole world
  if( sz->x < 0 ) sz->x = stg_world(this)->width;
  if( sz->y < 0 ) sz->y = stg_world(this)->height;

  ENT_DEBUG2( "setting size to %.2f %.2f", sz->x, sz->y );
  
  // if the new position is different, call SetProperty to make the change.
  if( memcmp( &this->size, sz, sizeof(stg_size_t)) != 0 )
    {
      this->UnMap(); // undraw myself	
      memcpy( &this->size, sz, sizeof(stg_size_t) );	
      this->Map(); // redraw myself  
    }
}

///////////////////////////////////////////////////////////////////////////
// Get the entitys pose in the parent cs
void CEntity::GetPose( stg_pose_t* pose )
{
  g_assert( pose );
  memcpy( pose, &this->pose_local, sizeof(stg_pose_t) );
}

///////////////////////////////////////////////////////////////////////////
// Get the entity's origin in the parent cs
void CEntity::GetOrigin( stg_pose_t* pose )
{
  g_assert( pose );
  memcpy( pose, &this->pose_origin, sizeof(stg_pose_t) );
}

////////////////////////////////////////////////////////////////////////////
// Set the entity's velocity in the global cs
void CEntity::SetVelocity( stg_velocity_t* vel )
{ 
  //ENT_DEBUG3( "setting velocity to [%.2f %.2f %.2f]", vel->x, vel->y, vel->a );
  memcpy( &this->velocity, vel, sizeof(stg_velocity_t) );
}

////////////////////////////////////////////////////////////////////////////
// Get the entity's velocity in the global cs
void CEntity::GetVelocity( stg_velocity_t* vel )
{
  g_assert( vel );
  memcpy( vel, &this->velocity, sizeof(stg_velocity_t) );
}


///////////////////////////////////////////////////////////////////////////
// Set the entitys pose in the global cs
void CEntity::SetGlobalPose( stg_pose_t* pose)
{
  // Get the pose of our parent in the global cs
  stg_pose_t origin;
  memset( &origin, 0, sizeof(stg_pose_t) );
  
  if( stg_ent_parent(this) ) 
    stg_ent_parent(this)->GetGlobalPose( &origin );
  
  // Compute our pose in the local cs
  stg_pose_t lpose;
  double cosa = cos(origin.a);
  double sina = sin(origin.a);
  lpose.x =  (pose->x - origin.x) * cosa + (pose->y - origin.y) * sina;
  lpose.y = -(pose->x - origin.x) * sina + (pose->y - origin.y) * cosa;
  lpose.a = pose->a - origin.a;
  
  SetPose( &lpose );
}


///////////////////////////////////////////////////////////////////////////
// Get the entitys pose in the global cs
void CEntity::GetGlobalPose( stg_pose_t* pose )
{
  // Get the pose of our parent in the global cs
  stg_pose_t origin;
  memset( &origin, 0, sizeof(origin) );
  
  if( stg_ent_parent(this) ) 
    stg_ent_parent(this)->GetGlobalPose( &origin );
  
  // Compute our pose in the global cs
  pose->x = origin.x + this->pose_local.x * cos(origin.a) - this->pose_local.y * sin(origin.a);
  pose->y = origin.y + this->pose_local.x * sin(origin.a) + this->pose_local.y * cos(origin.a);
  pose->a = origin.a + this->pose_local.a;
}

//////////////////////////////////////////////////////////////////////////
// scale an array of rectangles so they fit in a unit square
void CEntity::NormalizeRects(  stg_rotrect_t* rects, int num )
{
  // assuming the rectangles fit in a square +/- one billion units
  double minx, miny, maxx, maxy;
  minx = miny = BILLION;
  maxx = maxy = -BILLION;
  
  for( int r=0; r<num; r++ )
    {
      // test the origin of the rect
      if( rects[r].x < minx ) minx = rects[r].x;
      if( rects[r].y < miny ) miny = rects[r].y;      
      if( rects[r].x > maxx ) maxx = rects[r].x;      
      if( rects[r].y > maxy ) maxy = rects[r].y;

      // test the extremes of the rect
      if( (rects[r].x+rects[r].w)  < minx ) 
	minx = (rects[r].x+rects[r].w);
      
      if( (rects[r].y+rects[r].h)  < miny ) 
	miny = (rects[r].y+rects[r].h);
      
      if( (rects[r].x+rects[r].w)  > maxx ) 
	maxx = (rects[r].x+rects[r].w);
      
      if( (rects[r].y+rects[r].h)  > maxy ) 
	maxy = (rects[r].y+rects[r].h);
    }
  
  // now normalize all lengths so that the rects all fit inside
  // rectangle from 0,0 to 1,1
  double scalex = maxx - minx;
  double scaley = maxy - miny;
  for( int r=0; r<num; r++ )
    { 
      rects[r].x = (rects[r].x - minx) / scalex;
      rects[r].y = (rects[r].y - miny) / scaley;
      rects[r].w = rects[r].w / scalex;
      rects[r].h = rects[r].h / scaley;
    }
}


//////////////////////////////////////////////////////////////////////
// set the rectangles that make up this ent's body
void CEntity::SetRects( stg_rotrect_t* new_rects, int new_rect_count  )
{	
  // delete any old rects
  if( this->rect_array )
    {
      RenderRects( false );
      g_array_free( this->rect_array, TRUE );
      this->rect_array = NULL;
    } 
  
  this->rect_array = g_array_new( FALSE, TRUE, 
				  sizeof(stg_rotrect_t) );
  
  g_assert( this->rect_array );

  this->rect_array = g_array_append_vals( this->rect_array,
					  new_rects, new_rect_count );
   
  ENT_DEBUG1( "set %d rects", this->rect_array->len );
  
  /*  for( size_t r=0; r < this->rect_array->len; r++ )
     {
      stg_rotrect_t* rect = &g_array_index( this->rect_array,stg_rotrect_t, r);
      
      printf( "rect %d (%.2f %.2f %.2f %.2f %.2f\n", 
	      r, rect->x, rect->y, rect->a, rect->w, rect->h ); 
    }
  */

  // if we asked for a border, add a unit rectangle outline
  if( this->border )
    {
      //ENT_DEBUG( "drawing BORDER" );

      // set the first rect as a unit square
      stg_rotrect_t brect;
      brect.x = 0.0;
      brect.y = 0.0;
      brect.a = 0.0;
      brect.w = 1.0;
      brect.h = 1.0;
      
      this->rect_array = g_array_append_vals( this->rect_array, &brect, 1 );
    }
  
  RenderRects( true );
}


//////////////////////////////////////////////////////////////////////////
// write the entity tree onto the console
void CEntity::Print( int fd, char* prefix )
{
  // TODO - write onto the fd, rather than stdout
  stg_pose_t pose;
  this->GetGlobalPose( &pose );

  printf( "%s id: %d name: %s global: [%.2f,%.2f,%.2f]"
	  " local: [%.2f,%.2f,%.2f] vision_return %d )", 
	  prefix,
	  this->id,
	  this->name->str,
	  pose.x, pose.y, pose.a,
	  pose_local.x, pose_local.y, pose_local.a,
	  this->vision_return );
	  
  // add an indent to the prefix 
  char* buf = new char[ strlen(prefix) + 1 ];
  sprintf( buf, "\t%s", prefix );

  //CHILDLOOP( ch )
    //ch->Print( fd, buf );
}

///////////////////////////////////////////////////////////////////
// Get a string describing the entity, suitable for GUI or console output
void CEntity::GetStatusString( char* buf, int buflen )
{
   stg_pose_t pose;
   this->GetGlobalPose( &pose );
   
   // check for overflow
   assert( -1 !=  snprintf( buf, buflen, 
			    "Pose(%.2f,%.2f,%.2f) Stage(%d:%s)",
			    pose.x, pose.y, pose.a, this->id, this->name->str ) );
}  

/////////////////////////////////////////////////////////////////////////
// draw the rectangles in the matrix
void CEntity::RenderRects( bool render )
{
  stg_rotrect_t glob;
  
  if( this->rect_array )
    {
      int r;
      for( r=0; r<(int)this->rect_array->len; r++ )
	{
	  GetGlobalRect( &glob, 
			 &g_array_index(this->rect_array, stg_rotrect_t, r) );
	  
	  this->GetMatrix()->SetRectangle( glob.x, glob.y, glob.a, 
					   glob.w, glob.h, this, render );
	}  
    }
}

///////////////////////////////////////////////////////////////////////////
// Generate scan data
void CEntity::UpdateRangers( void )
{   
  ENT_DEBUG( "updating rangers" );

  assert( this->rangers );
  
  if( this->rangers->len < 1 )
    PRINT_WARN1( "wierd! %d rangers", this->rangers->len );

  for( int t=0; t < (int)this->rangers->len; t++ )
    {
      stg_ranger_t* tran = &g_array_index( this->rangers, 
					       stg_ranger_t, t );

      g_assert( tran );
      
      // Get the global pose of the ranger
      //
      stg_pose_t pz;
      memcpy( &pz, &tran->pose, sizeof(stg_pose_t) );
      this->LocalToGlobal( &pz );
      
      // todo - use bounds_range.min

      CLineIterator lit( pz.x, pz.y, pz.a, tran->bounds_range.max, 
			 GetWorld()->matrix, 
			 PointToBearingRange );
      
      CEntity* ent;
      double range = tran->bounds_range.max;
      
      while( (ent = lit.GetNextEntity()) ) 
	{
	  // Ignore myself, things which are attached to me, and
	  // things that we are attached to (except root) The latter
	  // is useful if you want to stack beacons on the laser or
	  // the laser on somethine else.
	  if (ent == this || this->IsDescendent(ent) )//|| 
	      //(ent != this->GetRoot() && ent->IsDescendent(this)))
	    continue;
	  
	  // Stop looking when we see something
	  if(ent->laser_return != LaserTransparent) 
	    {
	      range = lit.GetRange();
	      break;
	    }	
	}
      
      tran->range = range;

      stg_gui_rangers_render( this ); 
    }

  ENT_DEBUG( "updating rangers complete" );
}

///////////////////////////////////////////////////////////////////////////
// Generate scan data
void  CEntity::UpdateLaserData( stg_laser_data_t* laser )
{ 
  // remember the initial data
  //stg_laser_data_t keep;
  //memcpy( &keep, laser, sizeof(keep) );
     
  // Get the pose of the laser in the global cs
  stg_pose_t pz;
  GetGlobalPose( &pz );
  
  // the laser geometry is the same as the entity's
  memcpy( &laser->pose, &this->pose_origin, sizeof(stg_pose_t) );
  memcpy( &laser->size, &this->size, sizeof(stg_size_t) );
  
  double scan_res = (laser->angle_max - laser->angle_min) / laser->sample_count;
  
  // Do each scan
  for (int s = 0; s < laser->sample_count;)
    {
      // Compute parameters of scan line
      double bearing = s * scan_res + laser->angle_min;
      double pth = pz.a + bearing;
      
      CLineIterator lit( pz.x, pz.y, pth, laser->range_max, 
			 this->GetWorld()->matrix, PointToBearingRange );
      
      CEntity* ent;
      double range = laser->range_max;
      
      while( (ent = lit.GetNextEntity()) ) 
	{
	  // Ignore ourself, things which are attached to us,
	  // and things that we are attached to (except root)
	  // The latter is useful if you want to stack beacons
	  // on the laser or the laser on somethine else.
	  if (ent == this || this->IsDescendent(ent) )// || 
	    //(ent != m_world->root && ent->IsDescendent(this)))
	    continue;
	  
	  // Stop looking when we see something
	  if(ent->laser_return != LaserTransparent) 
	    {
	      range = lit.GetRange();
	      break;
	    }	
	}
      
      laser->samples[s].range = range;
      laser->samples[s].reflectance = 1.0;
      
      s++;
    }
  
  // ask the gui to draw this data if it's different to last time
  //if( memcmp( laser, &keep, sizeof(keep) ) )
    stg_gui_laser_render( this );
}
