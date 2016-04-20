///////////////////////////////////////////////////////////////////////////////
// Calorimeter-driven track finding
// Pattern recognition only, passes results to CalTrkFit
// P.Murat, G.Pezzullo
// try to order routines alphabetically
///////////////////////////////////////////////////////////////////////////////
#include "fhiclcpp/ParameterSet.h"

#include "CalPatRec/inc/CalPatRecNew_module.hh"
#include "CalPatRec/inc/Ref.hh"

// framework
#include "art/Framework/Principal/Handle.h"
#include "GeometryService/inc/GeomHandle.hh"
#include "GeometryService/inc/DetectorSystem.hh"
#include "art/Framework/Core/ModuleMacros.h"
#include "art/Framework/Services/Optional/TFileService.h"

// conditions
#include "ConditionsService/inc/AcceleratorParams.hh"
#include "ConditionsService/inc/ConditionsHandle.hh"
#include "ConditionsService/inc/TrackerCalibrations.hh"
#include "GeometryService/inc/getTrackerOrThrow.hh"
#include "TTrackerGeom/inc/TTracker.hh"
#include "CalorimeterGeom/inc/DiskCalorimeter.hh"
#include "ConfigTools/inc/ConfigFileLookupPolicy.hh"
#include "KalmanTests/inc/KalFitResult.hh"

#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/median.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/moment.hpp>
#include <boost/algorithm/string.hpp>

#include "TVector2.h"

using namespace std;
using namespace boost::accumulators;
using CLHEP::HepVector;
using CLHEP::HepSymMatrix;
using CLHEP::Hep3Vector;

namespace mu2e {
//-----------------------------------------------------------------------------
// comparison functor for sorting by Z(wire)
//-----------------------------------------------------------------------------
  struct straw_zcomp : public binary_function<hitIndex,hitIndex,bool> {
    bool operator()(hitIndex const& h1, hitIndex const& h2) {

      mu2e::GeomHandle<mu2e::TTracker> handle;
      const TTracker* t = handle.get();
      const Straw* s1 = &t->getStraw(StrawIndex(h1._index));
      const Straw* s2 = &t->getStraw(StrawIndex(h2._index));

      return s1->getMidPoint().z() < s2->getMidPoint().z();
    }
  }; // a semicolumn here is required

//-----------------------------------------------------------------------------
// module constructor, parameter defaults are defiend in CalPatRec/fcl/prolog.fcl
//-----------------------------------------------------------------------------
  CalPatRecNew::CalPatRecNew(fhicl::ParameterSet const& pset) :
    _diagLevel   (pset.get<int>        ("diagLevel")),
    _debugLevel  (pset.get<int>        ("debugLevel")),
    _printfreq   (pset.get<int>        ("printFrequency")),
    _addhits     (pset.get<bool>       ("addhits")),
    _shLabel     (pset.get<string>("StrawHitCollectionLabel"        )),
    _shpLabel    (pset.get<string>("StrawHitPositionCollectionLabel")),
    _shfLabel    (pset.get<string>("StrawHitFlagCollectionLabel"    )),
    _ccmLabel    (pset.get<string>("caloClusterModuleLabel"         )),
    _crmLabel    (pset.get<string>("caloReadoutModuleLabel"         )),
    _chmccpLabel (pset.get<string>("calorimeterHitMCCrystalPtr"     )),

    //    _dtspecpar   (pset.get<string>("DeltaTSpectrumParams","nobackgroundnomarkovgoff")),
    _tsel        (pset.get<vector<string> >("TimeSelectionBits")),
    _hsel        (pset.get<vector<string> >("HelixFitSelectionBits")),
    _addsel      (pset.get<vector<string> >("AddHitSelectionBits",vector<string>{} )),
    _ksel        (pset.get<vector<string> >("KalmanFitSelectionBits")),
    _bkgsel      (pset.get<vector<string> >("BackgroundSelectionBits")),
    _addbkg      (pset.get<vector<string> >("AddHitBackgroundBits",vector<string>{})),
    _maxedep     (pset.get<double>("MaxStrawEDep",0.005)),
    _mindt       (pset.get<double>("DtMin")),
    _maxdt       (pset.get<double>("DtMax")),
    _maxnpeak        (pset.get<unsigned>("MaxNPeaks",50)),
    _minnhits        (pset.get<int>   ("MinNHits" ,20)),
    _tmin            (pset.get<double>("tmin")),
    _tmax            (pset.get<double>("tmax")),
    _tbin            (pset.get<double>("tbin"             ,20.0)),
    _minClusterEnergy(pset.get<double>("minClusterEnergy" )),
    _minClusterSize  (pset.get<int>("minClusterSize" )),
    _pitchAngle      (pset.get<double>("_pitchAngle"      ,0.67)),
    _tpart           ((TrkParticle::type)(pset.get<int>("fitparticle"))),
    _fdir            ((TrkFitDirection::FitDirection)(pset.get<int>("fitdirection"))),
    _hfit            (pset.get<fhicl::ParameterSet>("HelixFitHack",fhicl::ParameterSet())),
    _dtoffset        (pset.get<double>("dtOffset")),
    _fieldcorr       (pset.get<bool>("fieldCorrection",false))
  {
    produces<TrackSeedCollection>();
    produces<CalTimePeakCollection>();

    fHackData = new THackData("HackData","Hack Data");
    gROOT->GetRootFolder()->Add(fHackData);
//-----------------------------------------------------------------------------
// provide for interactive disanostics
//-----------------------------------------------------------------------------
    _ref = new Ref("CalPatRecNewRef","Ref to CalPatRecNew",&_hfit);

    TFolder* f_mu2e;

    f_mu2e = (TFolder*) gROOT->GetRootFolder()->FindObject("Mu2e");
    if (f_mu2e == NULL) f_mu2e = gROOT->GetRootFolder()->AddFolder("Mu2e","Mu2e Folder");

    if (f_mu2e) {
      _folder = f_mu2e->AddFolder("CalPatRecNew","CalPatRecNew Folder");
      _folder->Add(_ref);
    }

    fgTimeOffsets     = new SimParticleTimeOffset(pset.get<fhicl::ParameterSet>("TimeOffsets"));

    _helTraj = 0;
    _bfield  = 0;
  }

//-----------------------------------------------------------------------------
// destructor
//-----------------------------------------------------------------------------
  CalPatRecNew::~CalPatRecNew() {
    delete _ref;
    if (_helTraj) delete _helTraj;
    if (_bfield)  delete _bfield;
    //    delete fStopwatch;
  }

//-----------------------------------------------------------------------------
  void CalPatRecNew::beginJob(){

    if(_diagLevel > 0) bookHistograms();

    _eventid = 0;
  }

//-----------------------------------------------------------------------------
  void CalPatRecNew::beginRun(art::Run& ) {
    mu2e::GeomHandle<mu2e::TTracker> th;
    _tracker = th.get();

    mu2e::GeomHandle<mu2e::Calorimeter> ch;
    _calorimeter = ch.get();
					// calibrations

    mu2e::ConditionsHandle<TrackerCalibrations> tcal("ignored");
    _trackerCalib = tcal.operator ->();

  }

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
  void CalPatRecNew::bookHistograms() {
    art::ServiceHandle<art::TFileService> tfs;

    art::TFileDirectory hf_dir = tfs->mkdir("HelixFit");
    
    _hist.nseeds[0]          = tfs->make<TH1F>("nseeds0"  , "number of track candidates: all events", 21, -0.5, 20.5);
    _hist.nseeds[1]          = tfs->make<TH1F>("nseeds1"  , "number of track candidates: nhits > 15", 21, -0.5, 20.5);
    _hist.helixFit.nhits     = hf_dir.make<TH1F>("nhits" , "number of hits within a track candidate; nHits", 101, -0.5, 100.5);
    _hist.helixFit.radius[0] = hf_dir.make<TH1F>("radius0", "helix radius; r [mm]"                  , 401, -0.5, 400.5);
    _hist.helixFit.radius[1] = hf_dir.make<TH1F>("radius1", "helix radius nhits > 15; r [mm]"       , 401, -0.5, 400.5);
    _hist.helixFit.pT [0]    = hf_dir.make<TH1F>("pT0"    , "transverse momentum; pT [MeV/c]"       , 400, -0.5, 200.5);
    _hist.helixFit.p  [0]    = hf_dir.make<TH1F>("p0"     , "momentum; p [MeV/c]"                   , 400, -0.5, 200.5);
    _hist.helixFit.pT [1]    = hf_dir.make<TH1F>("pT1"    , "transverse momentum nhits > 15; pT [MeV/c]"       , 400, -0.5, 200.5);
    _hist.helixFit.p  [1]    = hf_dir.make<TH1F>("p1"     , "momentum nhits > 15; p [MeV/c]"                   , 400, -0.5, 200.5);
    _hist.helixFit.nhitsvspT = hf_dir.make<TH2F>("nhitsvspT","nhits vs pT", 100, 0, 100, 400, 0, 200);
    _hist.helixFit.nhitsvsp  = hf_dir.make<TH2F>("nhitsvsp" ,"nhits vs p" , 100, 0, 100, 400, 0, 200);

  }

//-----------------------------------------------------------------------------
// find the input data objects
//-----------------------------------------------------------------------------
  bool CalPatRecNew::findData(const art::Event& evt) {

    //    art::Handle<mu2e::StrawHitCollection> strawhitsH;
    if (evt.getByLabel(_shLabel, _strawhitsH)) {
      _shcol = _strawhitsH.product();
    }
    else {
      _shcol  = 0;
      printf(" >>> ERROR in CalPatRecNew::findData: StrawHitCollection with label=%s not found.\n",
             _shLabel.data());
    }

    art::Handle<mu2e::StrawHitPositionCollection> shposH;
    if (evt.getByLabel(_shpLabel,shposH)) {
      _shpcol = shposH.product();
    }
    else {
      _shpcol = 0;
      printf(" >>> ERROR in CalPatRecNew::findData: StrawHitPositionCollection with label=%s not found.\n",
             _shpLabel.data());
    }

    art::Handle<mu2e::StrawHitFlagCollection> shflagH;
    if (evt.getByLabel(_shfLabel,shflagH)) {
      _shfcol = shflagH.product();
    }
    else {
      _shfcol = 0;
      printf(" >>> ERROR in CalPatRecNew::findData: StrawHitFlagCollection with label=%s not found.\n",
             _shfLabel.data());
    }

    art::Handle<CaloClusterCollection> ccH;
    if (evt.getByLabel(_ccmLabel, ccH)) {
      _ccCollection = ccH.product();
    }
    else {
      _ccCollection = 0;
      printf(" >>> ERROR in CalPatRecNew::findData: CaloClusterCollection with label=%s not found.\n",
             _ccmLabel.data());
    }
//-----------------------------------------------------------------------------
// find list of MC hits - for debugging only
//-----------------------------------------------------------------------------
    art::Handle<mu2e::PtrStepPointMCVectorCollection> mcptrHandle;
    evt.getByLabel(_shLabel,"StrawHitMCPtr",mcptrHandle);
    if (mcptrHandle.isValid()) {
      _listOfMCStrawHits = (mu2e::PtrStepPointMCVectorCollection*) mcptrHandle.product();
    }
    else {
      _listOfMCStrawHits = NULL;
    }

//------------------------------------------------------------------------------------------
// Utility to match  cloHits with MCtruth, simParticles and StepPoints
//------------------------------------------------------------------------------------------

    //Get calorimeter readout hits (2 readout / crystal as of today)
    art::Handle<CaloHitCollection> caloHitsHandle;
    if (evt.getByLabel(_crmLabel, caloHitsHandle)){
      _chcol = caloHitsHandle.product();
    }else{
      _chcol = 0;
    }

    //Get stepPointMC for crystal readout hits
    art::Handle<PtrStepPointMCVectorCollection> mccaloptrHandle;
    if (evt.getByLabel(_crmLabel,_chmccpLabel,mccaloptrHandle)){
      _listOfMCCrystals = mccaloptrHandle.product();
    }else {
      _listOfMCCrystals = 0;
    }

 
//-----------------------------------------------------------------------------
// done
//-----------------------------------------------------------------------------
    return (_shcol != 0) && (_shfcol != 0) && (_shpcol != 0) && (_ccCollection != 0);
  }

//-----------------------------------------------------------------------------
// event entry point
//-----------------------------------------------------------------------------
  void CalPatRecNew::produce(art::Event& event ) {
    const char*               oname = "CalPatRecNew::produce";
    int                       nhits;
    int                       npeaks;

    static TrkDef             dummydef;
    static HelixDefHack       dummyhdef;

    static HelixFitHackResult dummyhfit(dummyhdef);

    static StrawHitFlag       esel(StrawHitFlag::energysel), flag;

    //    ConditionsHandle<AcceleratorParams> accPar("ignored");
    //    _mbtime = accPar->deBuncherPeriod;
    fgTimeOffsets->updateMap(event);

                                        // event printout
    _eventid = event.event();
    _iev     = event.id().event();

    if ((_iev%_printfreq) == 0) printf("[%s] : START event number %8i\n", oname,_iev);

    _tpeaks = new CalTimePeakCollection;
    
    unique_ptr<TrackSeedCollection>    outseeds(new TrackSeedCollection);
    unique_ptr<CalTimePeakCollection>  tpeaks  (_tpeaks);

    
    _flags = new StrawHitFlagCollection();
    unique_ptr<StrawHitFlagCollection> flags (_flags);

                                        // find the data
    if (!findData(event)) {
      printf("%s ERROR: No straw hits found, RETURN\n",oname);
                                                            goto END;
    }
//-----------------------------------------------------------------------------
// count the number of MC straw hits generated by the CE
//-----------------------------------------------------------------------------
    nhits = _shcol->size();

//-----------------------------------------------------------------------------
// all needed pieces of data have been found,
// tighten the energy cut and copy flags, clear
//-----------------------------------------------------------------------------
    for (int i=0; i<nhits; i++) {
      flag = _shfcol->at(i);
      if (_shcol->at(i).energyDep() > _maxedep && flag.hasAllProperties(esel)) {
        flag.clear(esel);
      }
      _flags->push_back(flag);
    }
//-----------------------------------------------------------------------------
// find the time peaks in the time spectrum of selected hits.
//-----------------------------------------------------------------------------
    findTimePeaks(_tpeaks);
//-----------------------------------------------------------------------------
// diagnostics
//-----------------------------------------------------------------------------
    if (_diagLevel > 0) {
      fillTimeDiag();
    }
//-----------------------------------------------------------------------------
// loop over found time peaks - for us, - "eligible" calorimeter clusters
//-----------------------------------------------------------------------------
    npeaks = _tpeaks->size();

    for (int ipeak=0; ipeak<npeaks; ipeak++) {
      CalTimePeak* tp = &_tpeaks->at(ipeak);

//-----------------------------------------------------------------------------
// create track definitions for the helix fit from this initial information
//-----------------------------------------------------------------------------
      HelixDefHack helixdef(_shcol,_shpcol,_flags,tp->_index,_tpart,_fdir);

      TrkDef             seeddef(helixdef);

                                        // track fitting objects for this peak

      HelixFitHackResult hf_result(helixdef);

//-----------------------------------------------------------------------------
// Step 1: pattern recognition. Find initial helical approximation of a track
//-----------------------------------------------------------------------------
      int rc = _hfit.findHelix(hf_result,tp);

      if (rc) {
//-----------------------------------------------------------------------------
// pattern recognition succeeded, the seed fit starts
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// convert the result to standard helix parameters, and initialize the seed definition helix
//-----------------------------------------------------------------------------

        HepVector hpar;
        HepVector hparerr;
        _hfit.helixParams(hf_result,hpar,hparerr);

        HepSymMatrix hcov = vT_times_v(hparerr);
//----------------------------------------------------------------------------------------
// work around missing default constructor
//-----------------------------------------------------------------------------
        if (_helTraj == 0)  _helTraj = new HelixTraj(hpar,hcov);
        else               *_helTraj = HelixTraj(hpar,hcov);

        seeddef.setHelix(*_helTraj);
//-----------------------------------------------------------------------------
// P.Murat: here hits are ordered by index - WHY?
// the Kalman fitter needs them ordered in Z(straw)
//-----------------------------------------------------------------------------
        std::vector<hitIndex> goodhits;

        _index = _hfit._xyzp;

        std::sort(_index.begin(), _index.end(), [ ]( const XYZPHack& lhs,
                                                     const XYZPHack& rhs )
                  {
                    return lhs._ind < rhs._ind;
                  } );

        _nindex = _index.size();

        for (int i=0; i< _nindex; ++i){
          if (_index[i].isOutlier()) continue;
          goodhits.push_back(_index[i]._ind);
        }
        seeddef.setIndices (goodhits);
  
	//fill seed information
	TrackSeed      tmpseed;
	initTrackSeed(tmpseed, seeddef, tp, _strawhitsH, event);
        outseeds->push_back(tmpseed);
 
      }

      
      
    }

//--------------------------------------------------------------------------------    
// fill diagnostic if needed
//--------------------------------------------------------------------------------
    if (_diagLevel > 0) {
      int   nseeds = outseeds->size();
      
      _hist.nseeds[0]->Fill(nseeds);
      
      double          radius(0), nhits(0), pT(0), p(0);
      int             nseedsCut0(0), nhitsMin(15);
      TrackSeed      *tmpseed;
      double          mm2MeV = 3./10.;

      for (int i=0; i<nseeds; ++i){
	tmpseed = &outseeds->at(i);

	radius  = 1./fabs(tmpseed->omega());
	nhits   = tmpseed->_selectedTrackerHits.size();

	pT      = mm2MeV*radius;
	p       = pT/std::cos( std::atan(tmpseed->tanDip()));

	if (nhits >= nhitsMin) {
	  ++nseedsCut0;
	  _hist.helixFit.pT[1]     ->Fill(pT);
	  _hist.helixFit.p [1]     ->Fill(p);
	  _hist.helixFit.radius[1] ->Fill(radius);
	}
  	

	_hist.helixFit.radius[0] ->Fill(radius);
	_hist.helixFit.nhits  ->Fill(nhits);
	_hist.helixFit.pT[0]   ->Fill(pT);
	_hist.helixFit.p [0]   ->Fill(p);
	
	_hist.helixFit.nhitsvspT ->Fill(nhits, pT);
	_hist.helixFit.nhitsvsp  ->Fill(nhits, p );
	
      }

      _hist.nseeds[1]->Fill(nseedsCut0);

      
    }

//-----------------------------------------------------------------------------
// put reconstructed tracks into the event record
//-----------------------------------------------------------------------------
  END:;
    event.put(std::move(outseeds));
    event.put(std::move(tpeaks));

  }

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
  void CalPatRecNew::endJob(){
    // does this cause the file to close?
    art::ServiceHandle<art::TFileService> tfs;
  }

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
  void CalPatRecNew::findTimePeaks(CalTimePeakCollection* TimePeakColl) {

    int                 ncl, nsh;
    double              time, dt, tof, zstraw, cl_time, stime;
    double              xcl, ycl, zcl/*, dz_cl*/;
    const CaloCluster*  cl;
    const StrawHit*     hit;
    const Straw*        straw;
    Hep3Vector          gpos, tpos;

    StrawHitFlag        energyFlag(StrawHitFlag::energysel);
    StrawHitFlag        timeFlag  (StrawHitFlag::timesel);
    StrawHitFlag        radiusFlag(StrawHitFlag::radsel);
    StrawHitFlag        deltaRayFlag(StrawHitFlag::delta);
    StrawHitFlag        isolatedFlag(StrawHitFlag::isolated);
//-----------------------------------------------------------------------------
// Loop over calorimeter clusters
//-----------------------------------------------------------------------------
    nsh   = _shcol->size();
    ncl   = _ccCollection->size();

    for (int ic=0; ic<ncl; ic++) {
      cl      = &_ccCollection->at(ic);

      if ( cl->energyDep() > _minClusterEnergy) {

        if ( (int(cl->size()) >= _minClusterSize) ) {

          cl_time = cl->time();
//-----------------------------------------------------------------------------
// convert cluster coordinates defined in the disk frame to the detector
// coordinate system
//-----------------------------------------------------------------------------
          gpos = _calorimeter->fromSectionFrameFF(cl->sectionId(),cl->cog3Vector());
          tpos = _calorimeter->toTrackerFrame(gpos);

          xcl     = tpos.x();
          ycl     = tpos.y();
          zcl     = tpos.z();

          //    dz_cl   = zcl; // -_tracker->z0();
          // create time peak
          CalTimePeak tpeak(cl,xcl,ycl,zcl);

          tpeak._shcol  = _shcol;
          tpeak._shfcol = _shfcol;
          tpeak._tmin   = cl_time+_mindt;
          tpeak._tmax   = cl_time+_maxdt;
//-----------------------------------------------------------------------------
// record hits in time with each peak, and accept them if they have a minimum # of hits
//-----------------------------------------------------------------------------
          stime = 0;
          mu2e::StrawHitFlag flag;
          // int   nhitsTimeWindow(0), nhitsHasTime(0), nhitsHasEnergy(0), nhitsHasRadius(0),
          //       nhitsNoDelta(0), nhitsNoIsolated(0);

          double meanDriftTime = 1.25/0.06;// half straw tube radius / drift velocity
	  //          int    gen_index, sim_id, vol_id;

          for(int istr=0; istr<nsh;++istr) {
            flag = _flags->at(istr);

            int hit_has_all_properties = flag.hasAllProperties(_hsel);
            int bgr_hit                = flag.hasAnyProperty(_bkgsel);

            // int hit_has_energy         = flag.hasAllProperties(energyFlag);
            // int hit_has_time           = flag.hasAllProperties(timeFlag);
            // int hit_has_radius         = flag.hasAllProperties(radiusFlag);

            // int deltaRay_hit           = flag.hasAnyProperty(deltaRayFlag);
            // int isolated_hit           = flag.hasAnyProperty(isolatedFlag);

            hit    = &_shcol->at(istr);
            time   = hit->time();
            straw  = &_tracker->getStraw(hit->strawIndex());
            zstraw = straw->getMidPoint().z();
//-----------------------------------------------------------------------------
// estimate time-of-flight and calculate residual between the predicted and the hit times
//-----------------------------------------------------------------------------
            tof = (zcl-zstraw)/sin(_pitchAngle)/CLHEP::c_light;
            dt  = cl_time-(time+tof-meanDriftTime);
//-----------------------------------------------------------------------------
// fill some diag histograms
//-----------------------------------------------------------------------------

            if ((dt < _maxdt) && (dt >= _mindt)) {

              if (hit_has_all_properties && !bgr_hit) {
                tpeak._index.push_back(istr);
                stime += time;
              }
            }
          }

          tpeak._tpeak = stime/(tpeak.NHits()+1.e-12);

          if (tpeak.NHits() > _minnhits)       TimePeakColl->push_back(tpeak);
          
        }
      }
    }
  }

//-----------------------------------------------------------------------------
// 2014-04-08 P.M.: I don't think this function is called any more
//-----------------------------------------------------------------------------
  void CalPatRecNew::createTimePeak(CalTimePeakCollection* TimePeakColl) {
// find the median time
    accumulator_set<double, stats<tag::median(with_p_square_quantile) > > tacc;
    unsigned nstrs = _shcol->size();
    double   time;

    for(unsigned istr=0; istr<nstrs;++istr){
      if(_flags->at(istr).hasAllProperties(_tsel) && !_flags->at(istr).hasAnyProperty(_bkgsel)) {
        time = _shcol->at(istr).time();
        tacc(time);
      }
    }

    int np = boost::accumulators::extract::count(tacc);
    if(np >= _minnhits){
      double mtime  = median(tacc);
      // create a time peak from the full subset of selected hits
      CalTimePeak tpeak(0, 0., 0., 0.);
      for(unsigned istr=0; istr<nstrs;++istr){
        if(_flags->at(istr).hasAllProperties(_tsel) && !_flags->at(istr).hasAnyProperty(_bkgsel)){
          tpeak._index.push_back(istr);
        }
      }
      tpeak._tpeak = mtime;
      TimePeakColl->push_back(tpeak);
    }
  }

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
  void CalPatRecNew::fillStrawDiag() {

  }

//-----------------------------------------------------------------------------
  void CalPatRecNew::fillTimeDiag() {

  }

//--------------------------------------------------------------------------------
  BField const&  CalPatRecNew::bField() const {
    if(_bfield == 0){
      GeomHandle<BFieldConfig> bfconf;
      if(_fieldcorr){
// create a wrapper around the mu2e field
        _bfield = new BaBarMu2eField();
      } else {
// create a fixed field using the nominal value
        GeomHandle<BFieldConfig> bfconf;
        _bfield=new BFieldFixed(bfconf->getDSUniformValue());
        assert(_bfield != 0);
      }
    }
    return *_bfield;
  }

//--------------------------------------------------------------------------------
  void CalPatRecNew::initTrackSeed(TrackSeed                             &TrkSeed, 
				   TrkDef                                &SeedDef  , 
				   CalTimePeak                           *TPeak    , 
				   art::Handle<mu2e::StrawHitCollection> &StrawhitsH,
				   art::Event& Event){

//      // get flight distance of z=0
//     double    t0flt = SeedDef.helix().zFlight(0.0);
//     // estimate the momentum at that point using the helix parameters.  This is
//     // assumed constant for this crude estimate
//     double    mom   = TrkMomCalculator::vecMom(SeedDef.helix(), bField(), t0flt).mag();
//     // compute the particle velocity
//     double    vflt  = SeedDef.particle().beta(mom)*CLHEP::c_light;
// //-----------------------------------------------------------------------------
// // Calculate the path length of the particle from the middle of the Tracker to the
// // calorimeter, TPeak->Z() is calculated wrt the tracker center
// //-----------------------------------------------------------------------------
//     double    path  = TPeak->ClusterZ()/SeedDef.helix().sinDip();

//     //Set T0
//     TrkSeed._t0    = TPeak->ClusterT0() + _dtoffset - path/vflt;

//     //Set dummy error value
//     TrkSeed._errt0 = 1.;

    //set helix parameters
    TrkSeed._fullTrkSeed._d0     = SeedDef.helix().d0();
    TrkSeed._fullTrkSeed._phi0   = SeedDef.helix().phi0();
    TrkSeed._fullTrkSeed._omega  = SeedDef.helix().omega();
    TrkSeed._fullTrkSeed._z0     = SeedDef.helix().z0();
    TrkSeed._fullTrkSeed._tanDip = SeedDef.helix().tanDip();
    
    for(int i=0;i<5;i++) {
      for(int j=0;j<5;j++) {
	TrkSeed._fullTrkSeed._covMtrx[i][j] = SeedDef.helixCovMatr()[i][j];
      }
    }
    
    int             shIndices = SeedDef.strawHitIndices().size();
    const mu2e::hitIndex *hIndex;

    for (int i=0; i<shIndices; ++i){
      hIndex = &SeedDef.strawHitIndices().at(i);
      TrkSeed._fullTrkSeed._selectedTrackerHitsIdx.push_back( mu2e::HitIndex( hIndex->_index, hIndex->_ambig) );
      TrkSeed._selectedTrackerHits.push_back( StrawHitPtr (StrawhitsH, hIndex->_index) );
    }
    
    const mu2e::CaloCluster *cluster = TPeak->Cluster();
    
    TrkSeed._t0      = cluster->time(); 
    TrkSeed._errt0 = cluster->cog3Vector().z();

  }


}

using mu2e::CalPatRecNew;
DEFINE_ART_MODULE(CalPatRecNew);
