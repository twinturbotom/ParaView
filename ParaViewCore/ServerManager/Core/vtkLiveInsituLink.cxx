/*=========================================================================

  Program:   ParaView
  Module:    $RCSfile$

  Copyright (c) Kitware, Inc.
  All rights reserved.
  See Copyright.txt or http://www.paraview.org/HTML/Copyright.html for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/
#include "vtkLiveInsituLink.h"

#include "vtkAlgorithm.h"
#include "vtkCommand.h"
#include "vtkExtractsDeliveryHelper.h"
#include "vtkMultiProcessStream.h"
#include "vtkNetworkAccessManager.h"
#include "vtkNew.h"
#include "vtkObjectFactory.h"
#include "vtkProcessModule.h"
#include "vtkPVConfig.h"
#include "vtkPVXMLElement.h"
#include "vtkPVXMLParser.h"
#include "vtkSmartPointer.h"
#include "vtkSMInsituStateLoader.h"
#include "vtkSMMessage.h"
#include "vtkSMProxy.h"
#include "vtkSMSessionProxyManager.h"
#include "vtkSocketController.h"
#include "vtkTrivialProducer.h"

#include <assert.h>
#include <map>
#include <string>
#include <vtksys/ios/sstream>
#include <vtksys/SystemTools.hxx>

namespace
{
  void InitializeConnection(void *localArg,
    void *vtkNotUsed(remoteArg),
    int vtkNotUsed(remoteArgLength),
    int vtkNotUsed(remoteProcessId))
    {
    vtkLiveInsituLink* self = reinterpret_cast<vtkLiveInsituLink*>(localArg);
    self->InsituProcessConnected(NULL);
    }

  void UpdateRMI(void *localArg,
    void *remoteArg, int vtkNotUsed(remoteArgLength), int vtkNotUsed(remoteProcessId))
    {
    vtkLiveInsituLink* self = reinterpret_cast<vtkLiveInsituLink*>(localArg);
    double time = *(reinterpret_cast<double*>(remoteArg));
    if (self->GetProcessType() == vtkLiveInsituLink::VISUALIZATION)
      {
      self->OnSimulationUpdate(time);
      }
    else
      {
      self->SimulationUpdate(time);
      }
    }

  void PostProcessRMI(void *localArg,
    void *remoteArg, int vtkNotUsed(remoteArgLength), int vtkNotUsed(remoteProcessId))
    {
    vtkLiveInsituLink* self = reinterpret_cast<vtkLiveInsituLink*>(localArg);
    double time = *(reinterpret_cast<double*>(remoteArg));
 
    if (self->GetProcessType() == vtkLiveInsituLink::VISUALIZATION)
      {
      self->OnSimulationPostProcess(time);
      }
    else
      {
      self->SimulationPostProcess(time);
      }
    }
}

class vtkLiveInsituLink::vtkInternals
{
public:
  struct Key
    {
    std::string Group;
    std::string Name;
    int Port;
    std::string ToString() const
      {
      vtksys_ios::ostringstream key;
      key << this->Group.c_str() << ":" << this->Name.c_str() << ":" <<
        this->Port;
      return key.str();
      }
    bool operator < (const Key& other) const
      {
      return this->Group < other.Group ||
        this->Name < other.Name ||
        this->Port < other.Port; 
      }
    Key() : Port(0) {}
    Key(const char* group, const char* name, int port):
      Group(group), Name(name), Port(port) {}
    };

  typedef std::map<Key, vtkSmartPointer<vtkTrivialProducer> > ExtractsMap;
  ExtractsMap Extracts;
};

vtkStandardNewMacro(vtkLiveInsituLink);
//----------------------------------------------------------------------------
vtkLiveInsituLink::vtkLiveInsituLink():
  Hostname(0),
  InsituPort(0),
  ProcessType(SIMULATION),
  ProxyId(0),
  InsituXMLStateChanged(false),
  ExtractsChanged(false),
  InsituXMLState(0),
  URL(0),
  Internals(new vtkInternals())
{
  this->SetHostname("localhost");
}

//----------------------------------------------------------------------------
vtkLiveInsituLink::~vtkLiveInsituLink()
{
  this->SetHostname(0);
  this->SetURL(0);

  delete []this->InsituXMLState;
  this->InsituXMLState = 0;

  delete this->Internals;
  this->Internals = NULL;
}

//----------------------------------------------------------------------------
void vtkLiveInsituLink::Initialize(vtkSMSessionProxyManager* pxm)
{
  if (this->Controller)
    {
    // already initialized. all's well.
    return;
    }

  this->CoprocessorProxyManager = pxm;
  this->ExtractsDeliveryHelper = vtkSmartPointer<vtkExtractsDeliveryHelper>::New();

  switch (this->ProcessType)
    {
  case VISUALIZATION:
    this->ExtractsDeliveryHelper->SetProcessIsProducer(false);
    this->InitializeVisualization();
    break;

  case SIMULATION:
    this->ExtractsDeliveryHelper->SetProcessIsProducer(true);
    this->InitializeSimulation();
    break;
    }
}

//----------------------------------------------------------------------------
void vtkLiveInsituLink::InitializeVisualization()
{
  // This method gets called on the DataServer nodes.
  // On the root-node, we need to setup the server-socket to accept connections
  // from VTK Insitu code.
  // On satellites, we need to setup MPI-RMI handlers to ensure that the
  // satellites respond to a VTK Insitu connection setup correctly.
  vtkProcessModule* pm = vtkProcessModule::GetProcessModule();
  int myId = pm->GetPartitionId();
  // int numProcs = pm->GetNumberOfLocalPartitions();

  cout << myId << ": InitializeVisualization" << endl;
  if (myId == 0)
    {
    // save the visualization session reference so that we can communicate back to
    // the client.
    this->VisualizationSession =
      vtkPVSessionBase::SafeDownCast(pm->GetActiveSession());

    vtkNetworkAccessManager* nam = pm->GetNetworkAccessManager();

    vtksys_ios::ostringstream url;
    url << "tcp://localhost:" << this->InsituPort << "?"
      << "listen=true&nonblocking=true&"
      << "handshake=paraview.insitu." << PARAVIEW_VERSION;
    this->SetURL(url.str().c_str());
    vtkMultiProcessController* controller = nam->NewConnection(this->URL);
    if (controller)
      {
      // controller would generally be NULL, however due to magically timing,
      // the insitu lib may indeed connect just as we setup the socket, so we
      // handle that case.
      this->InsituProcessConnected(controller);
      controller->Delete();
      }
    else
      {
      nam->AddObserver(vtkCommand::ConnectionCreatedEvent,
        this, &vtkLiveInsituLink::OnConnectionCreatedEvent);
      }
    }
  else
    {
    vtkMultiProcessController* parallelController =
      vtkMultiProcessController::GetGlobalController();

    // add callback to listen to "events" from root node.
    // the command channel between sim and vis nodes is only setup on the root
    // nodes (there are socket connections between satellites for data x'fer but
    // not for any other kind of communication.
    parallelController->AddRMICallback(&InitializeConnection, this,
      INITIALIZE_CONNECTION);
    parallelController->AddRMICallback(&UpdateRMI, this, UPDATE_RMI_TAG);
    parallelController->AddRMICallback(&PostProcessRMI, this,
      POSTPROCESS_RMI_TAG);
    }
}

//----------------------------------------------------------------------------
void vtkLiveInsituLink::InitializeSimulation()
{
  // vtkLiveInsituLink::Initialize() should not call this method unless
  // Controller==NULL.
  assert(this->Controller == NULL);

  vtkProcessModule* pm = vtkProcessModule::GetProcessModule();
  int myId = pm->GetPartitionId();
  int numProcs = pm->GetNumberOfLocalPartitions();

  if (!pm->GetSymmetricMPIMode() && numProcs > 1)
    {
    vtkErrorMacro(
      "Running in parallel without symmetric mode is not supported. "
      "Aborting for debugging purposes.");
    abort();
    }

  if (myId == 0)
    {
    vtkNetworkAccessManager* nam = pm->GetNetworkAccessManager();

    vtksys_ios::ostringstream url;
    url << "tcp://" << this->Hostname << ":" << this->InsituPort << "?"
      << "timeout=0&handshake=paraview.insitu." << PARAVIEW_VERSION;
    this->SetURL(url.str().c_str());
    vtkMultiProcessController* controller = nam->NewConnection(this->URL);
    if (numProcs > 1)
      {
      int connection_established = controller != NULL? 1 : 0;
      pm->GetGlobalController()->Broadcast(&connection_established, 1, 0);
      }
    if (controller)
      {
      // controller would generally be NULL, however due to magically timing,
      // the insitu lib may indeed connect just as we setup the socket, so we
      // handle that case.
      this->InsituProcessConnected(controller);
      controller->Delete();
      }
    // nothing to do, no server to connect to.
    }
  else
    {
    int connection_established = 0;
    pm->GetGlobalController()->Broadcast(&connection_established, 1, 0);
    if (connection_established)
      {
      this->InsituProcessConnected(NULL);
      }
    }
}

//----------------------------------------------------------------------------
// Callback on Visualization process when a simulation connects to it.
void vtkLiveInsituLink::OnConnectionCreatedEvent()
{
  vtkProcessModule* pm = vtkProcessModule::GetProcessModule();
  vtkNetworkAccessManager* nam = pm->GetNetworkAccessManager();
  vtkMultiProcessController* controller = nam->NewConnection(this->URL);
  if (controller)
    {
    this->InsituProcessConnected(controller);
    controller->Delete();
    }
}

//----------------------------------------------------------------------------
void vtkLiveInsituLink::InsituProcessConnected(vtkMultiProcessController* controller)
{
  assert(this->Controller == NULL);

  this->Controller = controller;

  vtkMultiProcessController* parallelController =
    vtkMultiProcessController::GetGlobalController();
  int numProcs = parallelController->GetNumberOfProcesses();
  int myId = parallelController->GetLocalProcessId();

  if (myId == 0)
    {
    assert(controller != NULL);
    }
  else
    {
    assert(controller == NULL);
    }

  switch (this->ProcessType)
    {
  case VISUALIZATION:
    // Visualization side is the "slave" side in this relationship. We listen to
    // commands from SIMULATION.
    if (myId == 0)
      {
      unsigned int size=0;
      controller->Receive(&size, 1, 1, 8000);

      delete [] this->InsituXMLState;
      this->InsituXMLState = new char[size + 1];
      controller->Receive(this->InsituXMLState, size, 1, 8001);
      this->InsituXMLState[size] = 0;
      this->InsituXMLStateChanged = false;

      // setup RMI callbacks.
      controller->AddRMICallback(&UpdateRMI, this, UPDATE_RMI_TAG);
      controller->AddRMICallback(&PostProcessRMI, this, POSTPROCESS_RMI_TAG);

      // setup M2N connection.
      int otherProcs;
      controller->Send(&numProcs, 1, 1, 8002);
      controller->Receive(&otherProcs, 1, 1, 8003);
      this->ExtractsDeliveryHelper->SetNumberOfVisualizationProcesses(numProcs);
      this->ExtractsDeliveryHelper->SetNumberOfSimulationProcesses(otherProcs);

      if (numProcs > 1)
        {
        parallelController->TriggerRMIOnAllChildren(INITIALIZE_CONNECTION);
        parallelController->Broadcast(&otherProcs, 1, 0);
        }
      }
    else
      {
      int otherProcs = 0;
      parallelController->Broadcast(&otherProcs, 1, 0);
      this->ExtractsDeliveryHelper->SetNumberOfVisualizationProcesses(numProcs);
      this->ExtractsDeliveryHelper->SetNumberOfSimulationProcesses(otherProcs);
      }

    // wait for each of the sim processes to setup a socket connection to the
    // vis nodes for data x'fer.
    if (myId < std::min(
        this->ExtractsDeliveryHelper->GetNumberOfVisualizationProcesses(),
        this->ExtractsDeliveryHelper->GetNumberOfSimulationProcesses()))
      {
      vtkSocketController* sim2vis = vtkSocketController::New();
      if (!sim2vis->WaitForConnection(this->InsituPort + 1 + myId))
        {
        abort();
        }
      this->ExtractsDeliveryHelper->SetSimulation2VisualizationController(
        sim2vis);
      sim2vis->Delete();
      }

    // notify client.
    if (this->VisualizationSession)
      {
      assert(myId == 0);
      vtkSMMessage message;
      message.set_global_id(this->ProxyId);
      message.set_location(vtkPVSession::CLIENT);
      // overloading ChatMessage for now.
      message.SetExtension(ChatMessage::author, CONNECTED);
      message.SetExtension(ChatMessage::txt, this->InsituXMLState);
      this->VisualizationSession->NotifyAllClients(&message);
      }
    break;

  case SIMULATION:
    if (myId ==0)
      {
      // send startup state to the visualization process.
      vtkPVXMLElement* xml = this->CoprocessorProxyManager->SaveXMLState();
      vtksys_ios::ostringstream xml_string;
      xml->PrintXML(xml_string, vtkIndent());
      xml->Delete();

      unsigned int size = static_cast<unsigned int>(xml_string.str().size());
      controller->Send(&size, 1, 1, 8000);
      controller->Send(xml_string.str().c_str(),
        static_cast<vtkIdType>(xml_string.str().size()), 1, 8001);

      // setup M2N connection.
      int otherProcs;
      controller->Receive(&otherProcs, 1, 1, 8002);
      controller->Send(&numProcs, 1, 1, 8003);
      this->ExtractsDeliveryHelper->SetNumberOfVisualizationProcesses(otherProcs);
      this->ExtractsDeliveryHelper->SetNumberOfSimulationProcesses(numProcs);
      if (numProcs > 1)
        {
        parallelController->Broadcast(&otherProcs, 1, 0);
        }
      }
    else
      {
      int otherProcs = 0;
      parallelController->Broadcast(&otherProcs, 1, 0);
      this->ExtractsDeliveryHelper->SetNumberOfVisualizationProcesses(otherProcs);
      this->ExtractsDeliveryHelper->SetNumberOfSimulationProcesses(numProcs);
      }

    // connect to the sim-nodes for data x'fer.
    if (myId < std::min(
        this->ExtractsDeliveryHelper->GetNumberOfVisualizationProcesses(),
        this->ExtractsDeliveryHelper->GetNumberOfSimulationProcesses()))
      {
      vtkSocketController* sim2vis = vtkSocketController::New();
      vtksys::SystemTools::Delay(1000);
      if (!sim2vis->ConnectTo(this->Hostname, this->InsituPort + 1 + myId))
        {
        abort();
        }
      this->ExtractsDeliveryHelper->SetSimulation2VisualizationController(
        sim2vis);
      sim2vis->Delete();
      }
    break;
    }
}

//----------------------------------------------------------------------------
void vtkLiveInsituLink::SimulationInitialize(vtkSMSessionProxyManager* pxm)
{
  assert(this->ProcessType == SIMULATION);

  this->Initialize(pxm);
}

//----------------------------------------------------------------------------
void vtkLiveInsituLink::SimulationUpdate(double time)
{
  assert(this->ProcessType == SIMULATION);

  vtkProcessModule* pm = vtkProcessModule::GetProcessModule();
  int myId = pm->GetPartitionId();
  int numProcs = pm->GetNumberOfLocalPartitions();

  char* buffer = NULL;
  int buffer_size = 0;

  vtkMultiProcessStream extractsMessage;
  extractsMessage << 0; // indicating nothing changed.

  if (myId == 0)
    {
    // steps to perform:
    // 1. Check with visualization root-node to see if it has new coprocessing
    //    state updates. If so receive them and broadcast to all satellites.
    // 2. Update the CoprocessorProxyManager using the most recent XML state we
    //    have.
    if (this->Controller)
      {
      // Notify the vis root-node.
      this->Controller->TriggerRMI(1, &time, static_cast<int>(sizeof(double)),
        UPDATE_RMI_TAG);

      // Get status of the state. Did it change? If so receive the state and
      // broadcast it to satellites.
      this->Controller->Receive(&buffer_size, 1, 1, 8010);
      if (buffer_size > 0)
        {
        cout << "receiving modified state from Vis" << endl;
        buffer = new char[buffer_size + 1];
        this->Controller->Receive(buffer, buffer_size, 1, 8011);
        buffer[buffer_size] = 0;
        }

      // Get the information about extracts. When the extracts have changed or
      // not is encoded in the stream itself.
      this->Controller->Receive(extractsMessage, 1, 8012);
      }

    if (numProcs > 1)
      {
      pm->GetGlobalController()->Broadcast(&buffer_size, 1, 0);
      if (buffer_size > 0)
        {
        pm->GetGlobalController()->Broadcast(buffer, buffer_size, 0);
        }
      pm->GetGlobalController()->Broadcast(extractsMessage, 0);
      }
    }
  else
    {
    assert(numProcs > 1);
    pm->GetGlobalController()->Broadcast(&buffer_size, 1, 0);
    if (buffer_size > 0)
      {
      buffer = new char[buffer_size + 1];
      pm->GetGlobalController()->Broadcast(buffer, buffer_size, 0);
      buffer[buffer_size] = 0;
      }
    pm->GetGlobalController()->Broadcast(extractsMessage, 0);
    }

  // ** here on, all the code is executed on all processes (root and
  // satellites).

  if (buffer && buffer_size > 0)
    {
    vtkNew<vtkPVXMLParser> parser;
    if (parser->Parse(buffer))
      {
      this->XMLState = parser->GetRootElement();
      }
    }
  delete[] buffer;

  // This assumes that the coprocessing pipeline is recreated at every timestep
  // and hence we need to reload the state for every timestep. This may be
  // changed once we stop re-creating the coprocessing pipeline.
  if (this->XMLState)
    {
    vtkNew<vtkSMInsituStateLoader> loader;
    loader->SetSessionProxyManager(this->CoprocessorProxyManager);
    this->CoprocessorProxyManager->LoadXMLState(this->XMLState, loader.GetPointer());
    }

  // Process information about extracts.
  int extracts_valid = 0;
  extractsMessage >> extracts_valid;
  if (extracts_valid)
    {
    assert(this->ExtractsDeliveryHelper.GetPointer() != NULL);
    this->ExtractsDeliveryHelper->ClearAllExtracts();
    int numberOfExtracts;
    extractsMessage >> numberOfExtracts;
    for (int cc=0; cc < numberOfExtracts; cc++)
      {
      std::string group, name;
      int port;
      extractsMessage >> group >> name >> port;

      vtkSMProxy* proxy = this->CoprocessorProxyManager->GetProxy(
        group.c_str(), name.c_str());
      if (proxy)
        {
        vtkAlgorithm* algo = vtkAlgorithm::SafeDownCast(
          proxy->GetClientSideObject());
        if (algo)
          {
          vtkInternals::Key key(group.c_str(), name.c_str(), port);
          this->ExtractsDeliveryHelper->AddExtractProducer(
            key.ToString().c_str(), algo->GetOutputPort(port));
          }
        else
          {
          vtkErrorMacro("No vtkAlgorithm: " << group.c_str() << ", " << name.c_str());
          }
        }
      else
        {
        vtkErrorMacro("No proxy: " << group.c_str() << ", " << name.c_str());
        }
      }
    }
}

//----------------------------------------------------------------------------
void vtkLiveInsituLink::SimulationPostProcess(double time)
{
  assert(this->ProcessType == SIMULATION);

  vtkProcessModule* pm = vtkProcessModule::GetProcessModule();
  int myId = pm->GetPartitionId();
  if (myId == 0 && this->Controller)
    {
    // notify vis root node that we are ready to ship extracts.
    this->Controller->TriggerRMI(1, &time, static_cast<int>(sizeof(double)),
      POSTPROCESS_RMI_TAG);
    }

  if (this->ExtractsDeliveryHelper)
    {
    // We're done coprocessing. Deliver the extracts to the visualization
    // processes.
    this->ExtractsDeliveryHelper->Update();
    }
}

//----------------------------------------------------------------------------
void vtkLiveInsituLink::OnSimulationUpdate(double time)
{
  assert(this->ProcessType == VISUALIZATION);

  // this method get called on:
  // - root node when "sim" notifies the root viz node.
  // - satellizes when "root" viz node notifies the satellizes.
  cout << "vtkLiveInsituLink::OnSimulationUpdate: " << time << endl;

  vtkProcessModule* pm = vtkProcessModule::GetProcessModule();
  int myId = pm->GetPartitionId();
  if (myId == 0 && pm->GetNumberOfLocalPartitions() > 1)
    {
    pm->GetGlobalController()->TriggerRMIOnAllChildren(
      &time, static_cast<int>(sizeof(double)), UPDATE_RMI_TAG);
    }

  if (myId == 0)
    {
    // The steps to perform are:
    // 1. Send coprocessing pipeline state to the simulation root node, if the
    //    state has changed since the last time we sent it.
    // 2. Send information about extracts to the simulation root, if the
    //    requested extracts has changed.

    int xml_state_size = 0;
    if (this->InsituXMLStateChanged)
      {
      xml_state_size = strlen(this->InsituXMLState);
      }
    // xml_state_size of 0 indicates that there are not updates to the state.
    // the CoProcessor simply uses the state it received most recently.
    this->Controller->Send(&xml_state_size, 1, 1, 8010);
    if (xml_state_size > 0)
      {
      cout << "Sending modified state to simulation" << endl;
      this->Controller->Send(this->InsituXMLState, xml_state_size, 1, 8011);
      }

    vtkMultiProcessStream extractsMessage;
    if (this->ExtractsChanged)
      {
      extractsMessage << 1;
      extractsMessage << static_cast<int>(this->Internals->Extracts.size());
      for (vtkInternals::ExtractsMap::iterator iter=this->Internals->Extracts.begin();
        iter != this->Internals->Extracts.end(); ++iter)
        {
        extractsMessage << iter->first.Group << iter->first.Name << iter->first.Port;
        }
      }
    else
      {
      extractsMessage << 0;
      }
    this->Controller->Send(extractsMessage, 1, 8012);
    }
  else
    {
    // There's nothing to do on satellites for this call right now.
    }

  this->InsituXMLStateChanged = false;
  this->ExtractsChanged = false;
}

//----------------------------------------------------------------------------
void vtkLiveInsituLink::OnSimulationPostProcess(double time)
{
  assert(this->ProcessType == VISUALIZATION);

  vtkProcessModule* pm = vtkProcessModule::GetProcessModule();
  int myId = pm->GetPartitionId();
  if (myId == 0 && pm->GetNumberOfLocalPartitions() > 1)
    {
    pm->GetGlobalController()->TriggerRMIOnAllChildren(
      &time, static_cast<int>(sizeof(double)), POSTPROCESS_RMI_TAG);
    }
  
  cout << "vtkLiveInsituLink::OnSimulationPostProcess: " << time << endl;

  // Obtains extracts from the simulations processes.
  this->ExtractsDeliveryHelper->Update();

  // notify the client that updated data is available.
  if (this->VisualizationSession)
    {
    assert (myId == 0);

    // here we may let the client know exactly what extracts were updated, if
    // all were not updated. Currently we just assume all extracts are
    // redelivered and modified.
    vtkSMMessage message;
    message.set_global_id(this->ProxyId);
    message.set_location(vtkPVSession::CLIENT);
    // overloading ChatMessage for now.
    message.SetExtension(ChatMessage::author, NEXT_TIMESTEP_AVAILABLE);
    message.SetExtension(ChatMessage::txt, "");
    this->VisualizationSession->NotifyAllClients(&message);
    }
}

//----------------------------------------------------------------------------
void vtkLiveInsituLink::RegisterExtract(vtkTrivialProducer* producer,
    const char* groupname, const char* proxyname, int portnumber)
{
  assert(this->ProcessType == VISUALIZATION);

  if (!this->ExtractsDeliveryHelper)
    {
    vtkWarningMacro("Connection to simulation has been dropped!!!");
    return;
    }

  cout << "Adding Extract: " << groupname << ", " << proxyname << endl;
  vtkInternals::Key key(groupname, proxyname, portnumber);
  this->Internals->Extracts[key] = producer;
  this->ExtractsChanged = true;
  this->ExtractsDeliveryHelper->AddExtractConsumer(
    key.ToString().c_str(), producer);
}

//----------------------------------------------------------------------------
void vtkLiveInsituLink::UnRegisterExtract(vtkTrivialProducer* producer)
{
  assert(this->ProcessType == VISUALIZATION);

  if (!this->ExtractsDeliveryHelper)
    {
    vtkWarningMacro("Connection to simulation has been dropped!!!");
    return;
    }

  for (vtkInternals::ExtractsMap::iterator iter=this->Internals->Extracts.begin();
    iter != this->Internals->Extracts.end(); ++iter)
    {
    if (iter->second.GetPointer() == producer)
      {
      this->ExtractsDeliveryHelper->RemoveExtractConsumer(
        iter->first.ToString().c_str());
      this->Internals->Extracts.erase(iter);
      this->ExtractsChanged = true;
      break;
      }
    }
}

//----------------------------------------------------------------------------
void vtkLiveInsituLink::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
}
