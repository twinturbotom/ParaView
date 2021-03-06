/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkPVImageSliceMapper.cxx

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/
#include "vtkPVImageSliceMapper.h"


#include "vtkObjectFactory.h"
#include "vtkInformation.h"
#include "vtkImageData.h"
#include "vtkCommand.h"
#include "vtkRenderer.h"
#include "vtkRenderWindow.h"
#include "vtkScalarsToColors.h"
#include "vtkStreamingDemandDrivenPipeline.h"
#include "vtkExecutive.h"
#include "vtkDataArray.h"
#include "vtkScalarsToColors.h"

#ifdef VTKGL2
# include "vtkCellArray.h"
# include "vtkDataSetAttributes.h"
# include "vtkExtractVOI.h"
# include "vtkFloatArray.h"
# include "vtkNew.h"
# include "vtkOpenGLPolyDataMapper.h"
# include "vtkOpenGLTexture.h"
# include "vtkPoints.h"
# include "vtkPointData.h"
# include "vtkPolyData.h"
# include "vtkProperty.h"
# include "vtkTextureObject.h"
# include "vtkTrivialProducer.h"


// no-op  just here to shut up python wrapping
class vtkPainter : public vtkObject {};
//-----------------------------------------------------------------------------
void vtkPVImageSliceMapper::SetPainter(vtkPainter* )
{
}

#else

# include "vtkTexturePainter.h"

//-----------------------------------------------------------------------------
class vtkPVImageSliceMapper::vtkObserver : public vtkCommand
{
public:
  static vtkObserver* New()
    { return new vtkObserver; }

  virtual void Execute(vtkObject* caller, unsigned long event, void*)
    {
    vtkPainter* p = vtkPainter::SafeDownCast(caller);
    if (this->Target && p && event == vtkCommand::ProgressEvent)
      {
      this->Target->UpdateProgress(p->GetProgress());
      }
    }
  vtkObserver()
    {
    this->Target = 0;
    }
  vtkPVImageSliceMapper* Target;
};

#endif

vtkStandardNewMacro(vtkPVImageSliceMapper);
//----------------------------------------------------------------------------
vtkPVImageSliceMapper::vtkPVImageSliceMapper()
{
  this->Piece = 0;
  this->NumberOfPieces = 1;
  this->NumberOfSubPieces = 1;
  this->GhostLevel = 0;

  this->Slice = 0;
  this->SliceMode = XY_PLANE;
  this->UseXYPlane = 0;

#ifdef VTKGL2
  this->Texture = vtkOpenGLTexture::New();
  this->Texture->RepeatOff();
  vtkNew<vtkPolyData> polydata;
  vtkNew<vtkPoints> points;
  points->SetNumberOfPoints(4);
  polydata->SetPoints(points.Get());

  vtkNew<vtkCellArray> tris;
  tris->InsertNextCell(3);
  tris->InsertCellPoint(0);
  tris->InsertCellPoint(1);
  tris->InsertCellPoint(2);
  tris->InsertNextCell(3);
  tris->InsertCellPoint(0);
  tris->InsertCellPoint(2);
  tris->InsertCellPoint(3);
  polydata->SetPolys(tris.Get());

  vtkNew<vtkFloatArray> tcoords;
  tcoords->SetNumberOfComponents(2);
  tcoords->SetNumberOfTuples(4);
  tcoords->SetTuple2(0, 0.0, 0.0);
  tcoords->SetTuple2(1, 1.0, 0.0);
  tcoords->SetTuple2(2, 1.0, 1.0);
  tcoords->SetTuple2(3, 0.0, 1.0);
  polydata->GetPointData()->SetTCoords(tcoords.Get());

  vtkNew<vtkTrivialProducer> prod;
  prod->SetOutput(polydata.Get());
  vtkNew<vtkOpenGLPolyDataMapper> polyDataMapper;
  polyDataMapper->SetInputConnection(prod->GetOutputPort());
  this->PolyDataActor = vtkActor::New();
  this->PolyDataActor->SetMapper(polyDataMapper.Get());
  this->PolyDataActor->SetTexture(this->Texture);
  this->PolyDataActor->GetProperty()->SetAmbient(1.0);
  this->PolyDataActor->GetProperty()->SetDiffuse(0.0);

#else
  this->Observer = vtkObserver::New();
  this->Observer->Target = this;
  this->Painter = 0;

  this->PainterInformation = vtkInformation::New();
  vtkTexturePainter* painter = vtkTexturePainter::New();
  this->SetPainter(painter);
  painter->Delete();
#endif
}

//----------------------------------------------------------------------------
vtkPVImageSliceMapper::~vtkPVImageSliceMapper()
{
#ifdef VTKGL2
  this->Texture->Delete();
  this->Texture = NULL;
  this->PolyDataActor->Delete();
  this->PolyDataActor = NULL;
#else
  this->SetPainter(NULL);

  this->Observer->Target = 0;
  this->Observer->Delete();
  this->PainterInformation->Delete();
#endif
}

#ifdef VTKGL2

//----------------------------------------------------------------------------
static int vtkGetDataDimension(int inextents[6])
{
  int dim[3];
  dim[0] = inextents[1] - inextents[0] + 1;
  dim[1] = inextents[3] - inextents[2] + 1;
  dim[2] = inextents[5] - inextents[4] + 1;
  int dimensionality = 0;
  dimensionality += (dim[0]>1? 1 : 0);
  dimensionality += (dim[1]>1? 1 : 0);
  dimensionality += (dim[2]>1? 1 : 0);
  return dimensionality;
}

static const int XY_PLANE_QPOINTS_INDICES[] =
{0, 2, 4, 1, 2, 4, 1, 3, 4, 0, 3, 4};
static const int YZ_PLANE_QPOINTS_INDICES[] =
{0, 2, 4, 0, 3, 4, 0, 3, 5, 0, 2, 5};
static const int XZ_PLANE_QPOINTS_INDICES[] =
{0, 2, 4, 1, 2, 4, 1, 2, 5, 0, 2, 5};

static const int *XY_PLANE_QPOINTS_INDICES_ORTHO =
XY_PLANE_QPOINTS_INDICES;
static const int YZ_PLANE_QPOINTS_INDICES_ORTHO[] =
{2, 4, 0, 3, 4, 0, 3, 5, 0, 2, 5, 0};
static const int XZ_PLANE_QPOINTS_INDICES_ORTHO[] =
{ 4, 0, 2, 4, 1, 2, 5, 1, 2, 5, 0, 2 };


int vtkPVImageSliceMapper::SetupScalars(vtkImageData* input)
{
  // Based on the scalar mode, scalar array, scalar id,
  // we need to tell the vtkTexture to use the appropriate scalars.
  int cellFlag = 0;
  vtkDataArray* scalars = vtkAbstractMapper::GetScalars(input,
    this->ScalarMode,
    this->ArrayName? VTK_GET_ARRAY_BY_NAME : VTK_GET_ARRAY_BY_ID,
    this->ArrayId,
    this->ArrayName,
    cellFlag);

  if (!scalars)
    {
    vtkWarningMacro("Failed to locate selected scalars. Will use image "
      "scalars by default.");
    // If not scalar array specified, simply use the point data (the cell
    // data) scalars.
    this->Texture->SetInputArrayToProcess(0, 0, 0,
      vtkDataObject::FIELD_ASSOCIATION_POINTS_THEN_CELLS,
      vtkDataSetAttributes::SCALARS);
    cellFlag = 0;
    }
  else
    {
    // Pass the scalar array choice to the texture.
    this->Texture->SetInputArrayToProcess(0, 0, 0,
      (cellFlag? vtkDataObject::FIELD_ASSOCIATION_CELLS:
       vtkDataObject::FIELD_ASSOCIATION_POINTS),
      scalars->GetName());
    }
  return cellFlag;
}


//----------------------------------------------------------------------------
void vtkPVImageSliceMapper::RenderInternal(vtkRenderer *renderer,
                                           vtkActor *vtkNotUsed(actor))
{
  vtkImageData* input = vtkImageData::SafeDownCast(this->GetInput());
  if (this->UpdateTime < input->GetMTime() || this->UpdateTime < this->MTime)
    {
    this->UpdateTime.Modified();
    int sliceDescription = 0;
    int inextent[6];
    int outextent[6];
    // we deliberately use whole extent here. So on processes where the slice is
    // not available, the vtkExtractVOI filter will simply yield an empty
    // output.
    int *wext = this->GetInputInformation(0, 0)->Get(
      vtkStreamingDemandDrivenPipeline::WHOLE_EXTENT());
    memcpy(inextent, wext, 6*sizeof(int));
    memcpy(outextent, inextent, sizeof(int)*6);
    int numdims = ::vtkGetDataDimension(inextent);
    int dims[3];
    dims[0] = inextent[1] - inextent[0] + 1;
    dims[1] = inextent[3] - inextent[2] + 1;
    dims[2] = inextent[5] - inextent[4] + 1;

    // Based on the scalar mode, scalar array, scalar id,
    // we need to tell the vtkTexture to use the appropriate scalars.
    int cellFlag = this->SetupScalars(input);

    // Determine the VOI to extract:
    // * If the input image is 3D, then we respect the slice number and slice
    // direction the user recommended.
    // * If the input image is 2D, we simply show the input image slice.
    // * If the input image is 1D, we raise an error.
    if (numdims==3)
      {
      int slice = this->Slice;
      // clamp the slice number at min val.
      slice = (slice < 0)? 0 : slice;

      // if cell centered, then dimensions reduces by 1.
      int curdim = cellFlag? (dims[this->SliceMode]-1) :
        dims[this->SliceMode];

      // clamp the slice number at max val.
      slice = (slice >= curdim)? curdim-1: slice;

      if (this->SliceMode == XY_PLANE) // XY plane
        {
        outextent[4] = outextent[5] = outextent[4]+slice;
        sliceDescription = VTK_XY_PLANE;
        }
      else if (this->SliceMode == YZ_PLANE) // YZ plane
        {
        outextent[0] = outextent[1] = outextent[0] + slice;
        sliceDescription = VTK_YZ_PLANE;
        }
      else if (this->SliceMode == XZ_PLANE) // XZ plane
        {
        outextent[2] = outextent[3] = outextent[2] + slice;
        sliceDescription = VTK_XZ_PLANE;
        }
      }
    else if (numdims==2)
      {
      if (inextent[4] == inextent[5]) //XY plane
        {
        //nothing to change.
        sliceDescription = VTK_XY_PLANE;
        }
      else if (inextent[0] == inextent[1]) /// YZ plane
        {
        sliceDescription = VTK_YZ_PLANE;
        }
      else if (inextent[2] == inextent[3]) // XZ plane
        {
        sliceDescription = VTK_XZ_PLANE;
        }
      }
    else
      {
      vtkErrorMacro("Incorrect dimensionality.");
      return;
      }

    vtkNew<vtkImageData> clone;
    clone->ShallowCopy(input);

    vtkNew<vtkExtractVOI> extractVOI;
    extractVOI->SetVOI(outextent);
    extractVOI->SetInputData(clone.Get());
    extractVOI->Update();

    int evoi[6];
    extractVOI->GetOutput()->GetExtent(evoi);
    if (evoi[1] < evoi[0] && evoi[3] < evoi[2] && evoi[5] < evoi[4])
      {
      // if vtkExtractVOI did not produce a valid output, that means there's no
      // image slice to display.
      this->Texture->SetInputData(0);
      return;
      }

    // TODO: Here we would have change the input scalars if the user asked us to.
    // The LUT can be simply passed to the vtkTexture. It can handle scalar
    // mapping.
    this->Texture->SetInputConnection(extractVOI->GetOutputPort());
    double outputbounds[6];

    // TODO: vtkExtractVOI is not passing correct origin. Until that's fixed, I
    // will just use the input origin/spacing to compute the bounds.
    clone->SetExtent(evoi);
    clone->GetBounds(outputbounds);

    this->Texture->SetLookupTable(this->LookupTable);
    this->Texture->SetMapColorScalarsThroughLookupTable(
      this->ColorMode == VTK_COLOR_MODE_MAP_SCALARS ? 1 : 0);

    if (cellFlag)
      {
      // Structured bounds are point bounds. Shrink them to reflect cell
      // center bounds.
      // i.e move min bounds up by spacing/2 in that direction
      //     and move max bounds down by spacing/2 in that direction.
      double spacing[3];
      input->GetSpacing(spacing); // since spacing doesn't change, we can use
                                  // input spacing directly.
      for (int dir=0; dir < 3; dir++)
        {
        double& min = outputbounds[2*dir];
        double& max = outputbounds[2*dir+1];
        if (min+spacing[dir] <= max)
          {
          min += spacing[dir]/2.0;
          max -= spacing[dir]/2.0;
          }
        else
          {
          min = max = (min + spacing[dir]/2.0);
          }
        }
      }

    const int *indices = NULL;
    switch (sliceDescription)
      {
    case VTK_XY_PLANE:
      indices = XY_PLANE_QPOINTS_INDICES;
      if (this->UseXYPlane)
        {
        indices = XY_PLANE_QPOINTS_INDICES_ORTHO;
        outputbounds[4]=0;
        }
      break;

    case VTK_YZ_PLANE:
      indices = YZ_PLANE_QPOINTS_INDICES;
      if (this->UseXYPlane)
        {
        indices = YZ_PLANE_QPOINTS_INDICES_ORTHO;
        outputbounds[0]=0;
        }
      break;

    case VTK_XZ_PLANE:
      indices = XZ_PLANE_QPOINTS_INDICES;
      if (this->UseXYPlane)
        {
        indices = XZ_PLANE_QPOINTS_INDICES_ORTHO;
        outputbounds[2]=0;
        }
      break;
      }

    vtkPolyData *poly = vtkPolyDataMapper::SafeDownCast(
      this->PolyDataActor->GetMapper())->GetInput();
    vtkPoints *polyPoints = poly->GetPoints();

    for (int i = 0; i < 4; i++)
      {
      polyPoints->SetPoint(i, outputbounds[indices[i*3]],
        outputbounds[indices[3*i+1]], outputbounds[indices[3*i+2]]);
      }
    }

  if (!this->Texture->GetInput())
    {
    return;
    }

  this->Texture->Render(renderer);
  this->PolyDataActor->GetMapper()->Render(renderer, this->PolyDataActor);
  this->Texture->PostRender(renderer);
}

#else

//-----------------------------------------------------------------------------
void vtkPVImageSliceMapper::SetPainter(vtkPainter* p)
{
  if (this->Painter)
    {
    this->Painter->RemoveObservers(vtkCommand::ProgressEvent, this->Observer);
    this->Painter->SetInformation(0);
    }
  vtkSetObjectBodyMacro(Painter, vtkPainter, p);
   if (this->Painter)
    {
    this->Painter->AddObserver(vtkCommand::ProgressEvent, this->Observer);
    this->Painter->SetInformation(this->PainterInformation);
    }
}

//----------------------------------------------------------------------------
void vtkPVImageSliceMapper::UpdatePainterInformation()
{
  vtkInformation* info = this->PainterInformation;
  info->Set(vtkPainter::STATIC_DATA(), this->Static);

  // tell which array to color with.
  if (this->ScalarMode == VTK_SCALAR_MODE_USE_FIELD_DATA)
    {
    vtkErrorMacro("Field data coloring is not supported.");
    this->ScalarMode = VTK_SCALAR_MODE_DEFAULT;
    }

  if (this->ArrayAccessMode == VTK_GET_ARRAY_BY_ID)
    {
    info->Remove(vtkTexturePainter::SCALAR_ARRAY_NAME());
    info->Set(vtkTexturePainter::SCALAR_ARRAY_INDEX(), this->ArrayId);
    }
  else
    {
    info->Remove(vtkTexturePainter::SCALAR_ARRAY_INDEX());
    info->Set(vtkTexturePainter::SCALAR_ARRAY_NAME(), this->ArrayName);
    }
  info->Set(vtkTexturePainter::SCALAR_MODE(), this->ScalarMode);
  info->Set(vtkTexturePainter::LOOKUP_TABLE(), this->LookupTable);
  info->Set(vtkTexturePainter::USE_XY_PLANE(), this->UseXYPlane);

  // tell is we should map unsiged chars thorough LUT.
  info->Set(vtkTexturePainter::MAP_SCALARS(),
    (this->ColorMode == VTK_COLOR_MODE_MAP_SCALARS)? 1 : 0);

  // tell information about the slice.
  info->Set(vtkTexturePainter::SLICE(), this->Slice);
  switch(this->SliceMode)
    {
  case YZ_PLANE:
    info->Set(vtkTexturePainter::SLICE_MODE(), vtkTexturePainter::YZ_PLANE);
    break;

  case XZ_PLANE:
    info->Set(vtkTexturePainter::SLICE_MODE(), vtkTexturePainter::XZ_PLANE);
    break;

  case XY_PLANE:
    info->Set(vtkTexturePainter::SLICE_MODE(), vtkTexturePainter::XY_PLANE);
    break;
    }
}

#endif


//----------------------------------------------------------------------------
void vtkPVImageSliceMapper::ReleaseGraphicsResources (vtkWindow *win)
{
#ifdef VTKGL2
  this->Texture->ReleaseGraphicsResources(win);
#else
  this->Painter->ReleaseGraphicsResources(win);
#endif
  this->Superclass::ReleaseGraphicsResources(win);
}

//----------------------------------------------------------------------------
void vtkPVImageSliceMapper::Render(vtkRenderer* ren, vtkActor* act)
{
  if (this->Static)
    {
    this->RenderPiece(ren, act);
    }
  vtkImageData* input = this->GetInput();
  if (!input)
    {
    vtkErrorMacro("Mapper has no vtkImageData input.");
    return;
    }

  int nPieces = this->NumberOfSubPieces* this->NumberOfPieces;
  for (int cc=0; cc < this->NumberOfSubPieces; cc++)
    {
    int currentPiece = this->NumberOfSubPieces * this->Piece + cc;
    vtkStreamingDemandDrivenPipeline::SetUpdateExtent(this->GetInputInformation(),
      currentPiece, nPieces, this->GhostLevel);
    this->RenderPiece(ren, act);
    }

}

//----------------------------------------------------------------------------
void vtkPVImageSliceMapper::SetInputData(vtkImageData* input)
{
  this->SetInputDataInternal(0, input);
}

//----------------------------------------------------------------------------
vtkImageData* vtkPVImageSliceMapper::GetInput()
{
  return vtkImageData::SafeDownCast(this->GetExecutive()->GetInputData(0, 0));
}

//----------------------------------------------------------------------------
void vtkPVImageSliceMapper::Update(int port)
{
  if (!this->Static)
    {
    int currentPiece, nPieces = this->NumberOfPieces;
    vtkImageData* input = this->GetInput();

    // If the estimated pipeline memory usage is larger than
    // the memory limit, break the current piece into sub-pieces.
    if (input)
      {
      this->GetInputAlgorithm()->UpdateInformation();
      currentPiece = this->NumberOfSubPieces * this->Piece;
      vtkStreamingDemandDrivenPipeline::SetUpdateExtent(
        this->GetInputInformation(),
        currentPiece, this->NumberOfSubPieces*nPieces, this->GhostLevel);
      }

    this->Superclass::Update(port);
    }

#ifndef VTKGL2
  // Set the whole extent on the painter because it needs it internally
  // and it has no access to the pipeline information.
  vtkTexturePainter* ptr = vtkTexturePainter::SafeDownCast(this->GetPainter());

  int *wext = this->GetInputInformation(0, 0)->Get(
    vtkStreamingDemandDrivenPipeline::WHOLE_EXTENT());
  if (wext)
    {
    ptr->SetWholeExtent(wext);
    }
#endif
}


//----------------------------------------------------------------------------
double* vtkPVImageSliceMapper::GetBounds()
{
  static double bounds[6] = {-1.0, 1.0, -1.0, 1.0, -1.0, 1.0};
  vtkImageData* input = this->GetInput();
  if (!input)
    {
    return bounds;
    }

  this->Update();
  input->GetBounds(this->Bounds);
  if (this->UseXYPlane)
    {
    // When using XY plane, the image will be in XY plane placed at the origin,
    // hence we adjust the bounds.
    if (this->Bounds[0] == this->Bounds[1])
      {
      this->Bounds[0] = this->Bounds[2];
      this->Bounds[1] = this->Bounds[3];
      this->Bounds[2] = this->Bounds[4];
      this->Bounds[3] = this->Bounds[5];
      }
    else if (this->Bounds[2] == this->Bounds[3])
      {
      this->Bounds[0] = this->Bounds[4];
      this->Bounds[1] = this->Bounds[5];
      this->Bounds[2] = this->Bounds[0];
      this->Bounds[3] = this->Bounds[1];
      }
    else if (this->Bounds[5] == this->Bounds[5])
      {
      // nothing to do.
      }
    // We check for SliceMode only if the input is not already 2D, since slice
    // mode is applicable only for 3D images.
    else if (this->SliceMode == YZ_PLANE)
      {
      this->Bounds[0] = this->Bounds[2];
      this->Bounds[1] = this->Bounds[3];
      this->Bounds[2] = this->Bounds[4];
      this->Bounds[3] = this->Bounds[5];
      }
    else if (this->SliceMode == XZ_PLANE)
      {
      this->Bounds[0] = this->Bounds[4];
      this->Bounds[1] = this->Bounds[5];
      this->Bounds[2] = this->Bounds[0];
      this->Bounds[3] = this->Bounds[1];
      }

    this->Bounds[4] = this->Bounds[5] = 0.0;
    }

  return this->Bounds;
}

//----------------------------------------------------------------------------
void vtkPVImageSliceMapper::ShallowCopy(vtkAbstractMapper* mapper)
{
  vtkPVImageSliceMapper* idmapper = vtkPVImageSliceMapper::SafeDownCast(mapper);
  if (idmapper)
    {
    this->SetInputData(idmapper->GetInput());
    this->SetGhostLevel(idmapper->GetGhostLevel());
    this->SetNumberOfPieces(idmapper->GetNumberOfPieces());
    this->SetNumberOfSubPieces(idmapper->GetNumberOfSubPieces());
    }

  this->Superclass::ShallowCopy(mapper);
}

//----------------------------------------------------------------------------
int vtkPVImageSliceMapper::FillInputPortInformation(
  int vtkNotUsed(port), vtkInformation* info)
{
  info->Set(vtkAlgorithm::INPUT_REQUIRED_DATA_TYPE(), "vtkImageData");
  return 1;
}


//----------------------------------------------------------------------------
void vtkPVImageSliceMapper::RenderPiece(vtkRenderer* ren, vtkActor* actor)
{
  vtkImageData* input = this->GetInput();
  //
  // make sure that we've been properly initialized
  //
  if (ren->GetRenderWindow()->CheckAbortStatus())
    {
    return;
    }
  if ( input == NULL )
    {
    vtkErrorMacro(<< "No input!");
    return;
    }
  else
    {
    this->InvokeEvent(vtkCommand::StartEvent,NULL);
    if (!this->Static)
      {
      this->Update();
      }
    this->InvokeEvent(vtkCommand::EndEvent,NULL);

    vtkIdType numPts = input->GetNumberOfPoints();
    if (numPts == 0)
      {
      vtkDebugMacro(<< "No points!");
      return;
      }
    }
  // make sure our window is current
  ren->GetRenderWindow()->MakeCurrent();
  this->TimeToDraw = 0.0;
#ifdef VTKGL2
  this->RenderInternal(ren, actor);
#else
  if (this->Painter)
    {
    // Update Painter information if obsolete.
    if (this->PainterInformationUpdateTime < this->GetMTime())
      {
      this->UpdatePainterInformation();
      this->PainterInformationUpdateTime.Modified();
      }
    // Pass polydata if changed.
    if (this->Painter->GetInput() != input)
      {
      this->Painter->SetInput(input);
      }
    this->Painter->Render(ren, actor, 0xff,this->ForceCompileOnly==1);
    this->TimeToDraw = this->Painter->GetTimeToDraw();
    }
#endif

  // If the timer is not accurate enough, set it to a small
  // time so that it is not zero
  if ( this->TimeToDraw == 0.0 )
    {
    this->TimeToDraw = 0.0001;
    }

  this->UpdateProgress(1.0);
}

//----------------------------------------------------------------------------
void vtkPVImageSliceMapper::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);

  os << indent << "Piece : " << this->Piece << endl;
  os << indent << "NumberOfPieces : " << this->NumberOfPieces << endl;
  os << indent << "GhostLevel: " << this->GhostLevel << endl;
  os << indent << "Number of sub pieces: " << this->NumberOfSubPieces << endl;
}
