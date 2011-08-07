// -*- mode: cpp; mode: fold -*-
// Description								/*{{{*/
// $Id: packagemanager.cc,v 1.30 2003/04/27 03:04:15 doogie Exp $
/* ######################################################################

   Package Manager - Abstacts the package manager

   More work is needed in the area of transitioning provides, ie exim
   replacing smail. This can cause interesing side effects.

   Other cases involving conflicts+replaces should be tested. 
   
   ##################################################################### */
									/*}}}*/
// Include Files							/*{{{*/
#include <apt-pkg/packagemanager.h>
#include <apt-pkg/orderlist.h>
#include <apt-pkg/depcache.h>
#include <apt-pkg/error.h>
#include <apt-pkg/version.h>
#include <apt-pkg/acquire-item.h>
#include <apt-pkg/algorithms.h>
#include <apt-pkg/configuration.h>
#include <apt-pkg/sptr.h>
    
#include <apti18n.h>    
#include <iostream>
#include <fcntl.h> 
									/*}}}*/
using namespace std;

bool pkgPackageManager::SigINTStop = false;

// PM::PackageManager - Constructor					/*{{{*/
// ---------------------------------------------------------------------
/* */
pkgPackageManager::pkgPackageManager(pkgDepCache *pCache) : Cache(*pCache)
{
   FileNames = new string[Cache.Head().PackageCount];
   List = 0;
   Debug = _config->FindB("Debug::pkgPackageManager",false);
}
									/*}}}*/
// PM::PackageManager - Destructor					/*{{{*/
// ---------------------------------------------------------------------
/* */
pkgPackageManager::~pkgPackageManager()
{
   delete List;
   delete [] FileNames;
}
									/*}}}*/
// PM::GetArchives - Queue the archives for download			/*{{{*/
// ---------------------------------------------------------------------
/* */
bool pkgPackageManager::GetArchives(pkgAcquire *Owner,pkgSourceList *Sources,
				    pkgRecords *Recs)
{
   if (CreateOrderList() == false)
      return false;
   
   bool const ordering =
	_config->FindB("PackageManager::UnpackAll",true) ?
		List->OrderUnpack() : List->OrderCritical();
   if (ordering == false)
      return _error->Error("Internal ordering error");

   for (pkgOrderList::iterator I = List->begin(); I != List->end(); I++)
   {
      PkgIterator Pkg(Cache,*I);
      FileNames[Pkg->ID] = string();
      
      // Skip packages to erase
      if (Cache[Pkg].Delete() == true)
	 continue;

      // Skip Packages that need configure only.
      if (Pkg.State() == pkgCache::PkgIterator::NeedsConfigure && 
	  Cache[Pkg].Keep() == true)
	 continue;

      // Skip already processed packages
      if (List->IsNow(Pkg) == false)
	 continue;

      new pkgAcqArchive(Owner,Sources,Recs,Cache[Pkg].InstVerIter(Cache),
			FileNames[Pkg->ID]);
   }

   return true;
}
									/*}}}*/
// PM::FixMissing - Keep all missing packages				/*{{{*/
// ---------------------------------------------------------------------
/* This is called to correct the installation when packages could not
   be downloaded. */
bool pkgPackageManager::FixMissing()
{   
   pkgDepCache::ActionGroup group(Cache);
   pkgProblemResolver Resolve(&Cache);
   List->SetFileList(FileNames);

   bool Bad = false;
   for (PkgIterator I = Cache.PkgBegin(); I.end() == false; I++)
   {
      if (List->IsMissing(I) == false)
	 continue;
   
      // Okay, this file is missing and we need it. Mark it for keep 
      Bad = true;
      Cache.MarkKeep(I, false, false);
   }
 
   // We have to empty the list otherwise it will not have the new changes
   delete List;
   List = 0;
   
   if (Bad == false)
      return true;
   
   // Now downgrade everything that is broken
   return Resolve.ResolveByKeep() == true && Cache.BrokenCount() == 0;   
}
									/*}}}*/
// PM::ImmediateAdd - Add the immediate flag recursivly			/*{{{*/
// ---------------------------------------------------------------------
/* This adds the immediate flag to the pkg and recursively to the
   dependendies 
 */
void pkgPackageManager::ImmediateAdd(PkgIterator I, bool UseInstallVer, unsigned const int &Depth)
{
   DepIterator D;
   
   if(UseInstallVer)
   {
      if(Cache[I].InstallVer == 0)
	 return;
      D = Cache[I].InstVerIter(Cache).DependsList(); 
   } else {
      if (I->CurrentVer == 0)
	 return;
      D = I.CurrentVer().DependsList(); 
   }

   for ( /* nothing */  ; D.end() == false; D++)
      if (D->Type == pkgCache::Dep::Depends || D->Type == pkgCache::Dep::PreDepends)
      {
	 if(!List->IsFlag(D.TargetPkg(), pkgOrderList::Immediate))
	 {
	    if(Debug)
	       clog << OutputInDepth(Depth) << "ImmediateAdd(): Adding Immediate flag to " << D.TargetPkg() << " cause of " << D.DepType() << " " << I.Name() << endl;
	    List->Flag(D.TargetPkg(),pkgOrderList::Immediate);
	    ImmediateAdd(D.TargetPkg(), UseInstallVer, Depth + 1);
	 }
      }
   return;
}
									/*}}}*/
// PM::CreateOrderList - Create the ordering class			/*{{{*/
// ---------------------------------------------------------------------
/* This populates the ordering list with all the packages that are
   going to change. */
bool pkgPackageManager::CreateOrderList()
{
   if (List != 0)
      return true;
   
   delete List;
   List = new pkgOrderList(&Cache);
   
   NoImmConfigure = !_config->FindB("APT::Immediate-Configure",true);
   ImmConfigureAll = _config->FindB("APT::Immediate-Configure-All",false);
   
   if (Debug && ImmConfigureAll) 
      clog << "CreateOrderList(): Adding Immediate flag for all packages because of APT::Immediate-Configure-All" << endl;
   
   // Generate the list of affected packages and sort it
   for (PkgIterator I = Cache.PkgBegin(); I.end() == false; I++)
   {
      // Ignore no-version packages
      if (I->VersionList == 0)
	 continue;
      
      // Mark the package and its dependends for immediate configuration
      if ((((I->Flags & pkgCache::Flag::Essential) == pkgCache::Flag::Essential ||
	   (I->Flags & pkgCache::Flag::Important) == pkgCache::Flag::Important) &&
	  NoImmConfigure == false) || ImmConfigureAll)
      {
	 if(Debug && !ImmConfigureAll)
	    clog << "CreateOrderList(): Adding Immediate flag for " << I.Name() << endl;
	 List->Flag(I,pkgOrderList::Immediate);
	 
	 if (!ImmConfigureAll) {
	    // Look for other install packages to make immediate configurea
	    ImmediateAdd(I, true);
	  
	    // And again with the current version.
	    ImmediateAdd(I, false);
	 }
      }
      
      // Not interesting
      if ((Cache[I].Keep() == true || 
	  Cache[I].InstVerIter(Cache) == I.CurrentVer()) && 
	  I.State() == pkgCache::PkgIterator::NeedsNothing &&
	  (Cache[I].iFlags & pkgDepCache::ReInstall) != pkgDepCache::ReInstall &&
	  (I.Purge() != false || Cache[I].Mode != pkgDepCache::ModeDelete ||
	   (Cache[I].iFlags & pkgDepCache::Purge) != pkgDepCache::Purge))
	 continue;
      
      // Append it to the list
      List->push_back(I);      
   }
   
   return true;
}
									/*}}}*/
// PM::DepAlwaysTrue - Returns true if this dep is irrelevent		/*{{{*/
// ---------------------------------------------------------------------
/* The restriction on provides is to eliminate the case when provides
   are transitioning between valid states [ie exim to smail] */
bool pkgPackageManager::DepAlwaysTrue(DepIterator D)
{
   if (D.TargetPkg()->ProvidesList != 0)
      return false;
   
   if ((Cache[D] & pkgDepCache::DepInstall) != 0 &&
       (Cache[D] & pkgDepCache::DepNow) != 0)
      return true;
   return false;
}
									/*}}}*/
// PM::CheckRConflicts - Look for reverse conflicts			/*{{{*/
// ---------------------------------------------------------------------
/* This looks over the reverses for a conflicts line that needs early
   removal. */
bool pkgPackageManager::CheckRConflicts(PkgIterator Pkg,DepIterator D,
					const char *Ver)
{
   for (;D.end() == false; D++)
   {
      if (D->Type != pkgCache::Dep::Conflicts &&
	  D->Type != pkgCache::Dep::Obsoletes)
	 continue;

      // The package hasnt been changed
      if (List->IsNow(Pkg) == false)
	 continue;
      
      // Ignore self conflicts, ignore conflicts from irrelevent versions
      if (D.ParentPkg() == Pkg || D.ParentVer() != D.ParentPkg().CurrentVer())
	 continue;
      
      if (Cache.VS().CheckDep(Ver,D->CompareOp,D.TargetVer()) == false)
	 continue;

      if (EarlyRemove(D.ParentPkg()) == false)
	 return _error->Error("Reverse conflicts early remove for package '%s' failed",
			      Pkg.Name());
   }
   return true;
}
									/*}}}*/
// PM::ConfigureAll - Run the all out configuration			/*{{{*/
// ---------------------------------------------------------------------
/* This configures every package. It is assumed they are all unpacked and
   that the final configuration is valid. This is also used to catch packages
   that have not been configured when using ImmConfigureAll */
bool pkgPackageManager::ConfigureAll()
{
   pkgOrderList OList(&Cache);
   
   // Populate the order list
   for (pkgOrderList::iterator I = List->begin(); I != List->end(); I++)
      if (List->IsFlag(pkgCache::PkgIterator(Cache,*I),
		       pkgOrderList::UnPacked) == true)
	 OList.push_back(*I);
   
   if (OList.OrderConfigure() == false)
      return false;

   std::string const conf = _config->Find("PackageManager::Configure","all");
   bool const ConfigurePkgs = (conf == "all");

   // Perform the configuring
   for (pkgOrderList::iterator I = OList.begin(); I != OList.end(); I++)
   {
      PkgIterator Pkg(Cache,*I);
      
      /* Check if the package has been configured, this can happen if SmartConfigure
         calls its self */ 
      if (List->IsFlag(Pkg,pkgOrderList::Configured)) continue;

      if (ConfigurePkgs == true && SmartConfigure(Pkg) == false) {
         _error->Error("Internal error, packages left unconfigured. %s",Pkg.Name());
	 return false;
      }
      
      List->Flag(Pkg,pkgOrderList::Configured,pkgOrderList::States);
   }
   
   return true;
}
									/*}}}*/
// PM::SmartConfigure - Perform immediate configuration of the pkg	/*{{{*/
// ---------------------------------------------------------------------
/* This routine trys to put the system in a state where Pkg can be configured,
   this involves checking each of Pkg's dependanies and unpacking and 
   configuring packages where needed. */
bool pkgPackageManager::SmartConfigure(PkgIterator Pkg)
{
   if (Debug == true)
      clog << "SmartConfigure " << Pkg.Name() << endl;
      
   VerIterator const instVer = Cache[Pkg].InstVerIter(Cache);
      
   /* Because of the ordered list, most dependancies should be unpacked, 
      however if there is a loop this is not the case, so check for dependancies before configuring.
      This is done after the package installation as it makes it easier to deal with conflicts problems */
   bool Bad = false;
   for (DepIterator D = instVer.DependsList();
	D.end() == false; )
   {
      // Compute a single dependency element (glob or)
      pkgCache::DepIterator Start;
      pkgCache::DepIterator End;
      D.GlobOr(Start,End);
      
      if (End->Type == pkgCache::Dep::Depends) 
          Bad = true;

      // Check for dependanices that have not been unpacked, probably due to loops.
      while (End->Type == pkgCache::Dep::Depends) {
         PkgIterator DepPkg;
         VerIterator InstallVer;
         SPtrArray<Version *> VList = Start.AllTargets();
         
	 for (Version **I = VList; *I != 0; I++) {
	    VerIterator Ver(Cache,*I);
	    DepPkg = Ver.ParentPkg();
	    
	    if (!Bad) continue;
	       
	    InstallVer = VerIterator(Cache,Cache[DepPkg].InstallVer);
	    //VerIterator CandVer(Cache,Cache[DepPkg].CandidateVer);
	    
	    if (Debug) {
	       if (Ver==0) {
	          cout << "  Checking if " << Ver << " of " << DepPkg.Name() << " satisfies this dependancy" << endl;
	       } else {
	          cout << "  Checking if " << Ver.VerStr() << " of " << DepPkg.Name() << " satisfies this dependancy" << endl;
               }
            
               if (DepPkg.CurrentVer()==0) {
                  cout << "    CurrentVer " << DepPkg.CurrentVer() << " IsNow " << List->IsNow(DepPkg) << " NeedsNothing " << (DepPkg.State() == PkgIterator::NeedsNothing) << endl;
               } else { 
                  cout << "    CurrentVer " << DepPkg.CurrentVer().VerStr() << " IsNow " << List->IsNow(DepPkg) << " NeedsNothing " << (DepPkg.State() == PkgIterator::NeedsNothing) << endl;
               }
            
               if (InstallVer==0) {
                  cout << "    InstallVer " << InstallVer << endl;
               } else { 
                  cout << "    InstallVer " << InstallVer.VerStr() << endl; 
               }
               //if (CandVer != 0)
               //   cout << "    CandVer " << CandVer.VerStr() << endl; 

               cout << "    Keep " << Cache[DepPkg].Keep() << " Unpacked " << List->IsFlag(DepPkg,pkgOrderList::UnPacked) << " Configured " << List->IsFlag(DepPkg,pkgOrderList::Configured) << " Removed " << List->IsFlag(DepPkg,pkgOrderList::Removed) << endl;
               
            }

	    // Check if it satisfies this dependancy
	    if (DepPkg.CurrentVer() == Ver && List->IsNow(DepPkg) == true && 
		!List->IsFlag(DepPkg,pkgOrderList::Removed) && DepPkg.State() == PkgIterator::NeedsNothing)
	    {
	       Bad = false;
	       continue;
	    }
	    
	    if (Cache[DepPkg].InstallVer == *I) {
	       if (List->IsFlag(DepPkg,pkgOrderList::UnPacked)) {
	          if (!List->IsFlag(DepPkg,pkgOrderList::Loop)) {
	             List->Flag(Pkg,pkgOrderList::Loop);
	             Bad = !SmartConfigure(DepPkg);
	          } else {
	             Bad = false;
	          }
	       } else if (List->IsFlag(DepPkg,pkgOrderList::Configured)) {
	          Bad = false;
	       }
	       continue;
	    }
	 }
	 
	 if (InstallVer != 0 && Bad) {
	    Bad = false;
	    List->Flag(Pkg,pkgOrderList::Loop);
	    if (!List->IsFlag(DepPkg,pkgOrderList::Loop)) {
	       if (Debug) 
	          cout << "  Unpacking " << DepPkg.Name() << " to avoid loop" << endl;
	       SmartUnPack(DepPkg, true);
	    }
	 }
	 
	 if (Start==End) {
	    if (Bad && Debug) {
	       if (!List->IsFlag(DepPkg,pkgOrderList::Loop)) {
                  _error->Warning("Could not satisfy dependancies for %s",Pkg.Name());
               } 
	    }
	    break;
	       
	 } else {
            Start++;
         }
      }
   }

   static std::string const conf = _config->Find("PackageManager::Configure","all");
   static bool const ConfigurePkgs = (conf == "all" || conf == "smart");

   if (List->IsFlag(Pkg,pkgOrderList::Configured)) 
      return _error->Error("Internal configure error on '%s'. ",Pkg.Name(),1);

   if (ConfigurePkgs == true && Configure(Pkg) == false)
      return false;
      
   List->Flag(Pkg,pkgOrderList::Configured,pkgOrderList::States);

   if (Cache[Pkg].InstVerIter(Cache)->MultiArch == pkgCache::Version::Same)
      for (PkgIterator P = Pkg.Group().PackageList();
	   P.end() == false; P = Pkg.Group().NextPkg(P))
      {
	 if (Pkg == P || List->IsFlag(P,pkgOrderList::Configured) == true ||
	     Cache[P].InstallVer == 0 || (P.CurrentVer() == Cache[P].InstallVer &&
	      (Cache[Pkg].iFlags & pkgDepCache::ReInstall) != pkgDepCache::ReInstall))
	    continue;
	 SmartConfigure(P);
      }

   // Sanity Check
   if (List->IsFlag(Pkg,pkgOrderList::Configured) == false && Debug)
      _error->Warning(_("Could not perform immediate configuration on '%s'. "
			"Please see man 5 apt.conf under APT::Immediate-Configure for details. (%d)"),Pkg.Name(),1);

   return true;
}
									/*}}}*/
// PM::EarlyRemove - Perform removal of packages before their time	/*{{{*/
// ---------------------------------------------------------------------
/* This is called to deal with conflicts arising from unpacking */
bool pkgPackageManager::EarlyRemove(PkgIterator Pkg)
{
   if (List->IsNow(Pkg) == false)
      return true;
	 
   // Already removed it
   if (List->IsFlag(Pkg,pkgOrderList::Removed) == true)
      return true;
   
   // Woops, it will not be re-installed!
   if (List->IsFlag(Pkg,pkgOrderList::InList) == false)
      return false;

   // Essential packages get special treatment
   bool IsEssential = false;
   if ((Pkg->Flags & pkgCache::Flag::Essential) != 0)
      IsEssential = true;

   /* Check for packages that are the dependents of essential packages and 
      promote them too */
   if (Pkg->CurrentVer != 0)
   {
      for (DepIterator D = Pkg.RevDependsList(); D.end() == false &&
	   IsEssential == false; D++)
	 if (D->Type == pkgCache::Dep::Depends || D->Type == pkgCache::Dep::PreDepends)
	    if ((D.ParentPkg()->Flags & pkgCache::Flag::Essential) != 0)
	       IsEssential = true;
   }

   if (IsEssential == true)
   {
      if (_config->FindB("APT::Force-LoopBreak",false) == false)
	 return _error->Error(_("This installation run will require temporarily "
				"removing the essential package %s due to a "
				"Conflicts/Pre-Depends loop. This is often bad, "
				"but if you really want to do it, activate the "
				"APT::Force-LoopBreak option."),Pkg.Name());
   }
   
   bool Res = SmartRemove(Pkg);
   if (Cache[Pkg].Delete() == false)
      List->Flag(Pkg,pkgOrderList::Removed,pkgOrderList::States);
   
   return Res;
}
									/*}}}*/
// PM::SmartRemove - Removal Helper					/*{{{*/
// ---------------------------------------------------------------------
/* */
bool pkgPackageManager::SmartRemove(PkgIterator Pkg)
{
   if (List->IsNow(Pkg) == false)
      return true;

   List->Flag(Pkg,pkgOrderList::Configured,pkgOrderList::States);

   return Remove(Pkg,(Cache[Pkg].iFlags & pkgDepCache::Purge) == pkgDepCache::Purge);
   return true;
}
									/*}}}*/
// PM::SmartUnPack - Install helper					/*{{{*/
// ---------------------------------------------------------------------
/* This puts the system in a state where it can Unpack Pkg, if Pkg is allready
   unpacked, or when it has been unpacked, if Immediate==true it configures it. */
bool pkgPackageManager::SmartUnPack(PkgIterator Pkg)
{
   return SmartUnPack(Pkg, true);
}
bool pkgPackageManager::SmartUnPack(PkgIterator Pkg, bool const Immediate)
{
   if (Debug == true)
      clog << "SmartUnPack " << Pkg.Name() << endl;

   // Check if it is already unpacked
   if (Pkg.State() == pkgCache::PkgIterator::NeedsConfigure &&
       Cache[Pkg].Keep() == true)
   {
      List->Flag(Pkg,pkgOrderList::UnPacked,pkgOrderList::States);
      if (Immediate == true &&
	  List->IsFlag(Pkg,pkgOrderList::Immediate) == true)
	 if (SmartConfigure(Pkg) == false)
	    _error->Warning(_("Could not perform immediate configuration on already unpacked '%s'. "
			"Please see man 5 apt.conf under APT::Immediate-Configure for details."),Pkg.Name());
      return true;
   }
   
   VerIterator const instVer = Cache[Pkg].InstVerIter(Cache);

   /* PreUnpack Checks: This loop checks and attemps to rectify and problems that would prevent the package being unpacked.
      It addresses: PreDepends, Conflicts, Obsoletes and DpkgBreaks. Any resolutions that do not require it should 
      avoid configuration (calling SmartUnpack with Immediate=true), this is because any loops before Pkg is unpacked 
      can cause problems. This will be either dealt with if the package is configured as a dependancy of 
      Pkg (if and when Pkg is configured), or by the ConfigureAll call at the end of the for loop in OrderInstall. */
   for (DepIterator D = instVer.DependsList();
	D.end() == false; )
   {
      // Compute a single dependency element (glob or)
      pkgCache::DepIterator Start;
      pkgCache::DepIterator End;
      D.GlobOr(Start,End);
      
      while (End->Type == pkgCache::Dep::PreDepends)
      {
	 if (Debug)
	    clog << "PreDepends order for " << Pkg.Name() << std::endl;

	 // Look for possible ok targets.
	 SPtrArray<Version *> VList = Start.AllTargets();
	 bool Bad = true;
	 for (Version **I = VList; *I != 0 && Bad == true; I++)
	 {
	    VerIterator Ver(Cache,*I);
	    PkgIterator Pkg = Ver.ParentPkg();
	    
	    // See if the current version is ok
	    if (Pkg.CurrentVer() == Ver && List->IsNow(Pkg) == true && 
		Pkg.State() == PkgIterator::NeedsNothing)
	    {
	       Bad = false;
	       if (Debug)
		  clog << "Found ok package " << Pkg.Name() << endl;
	       continue;
	    }
	 }
	 
	 // Look for something that could be configured.
	 for (Version **I = VList; *I != 0 && Bad == true; I++)
	 {
	    VerIterator Ver(Cache,*I);
	    PkgIterator Pkg = Ver.ParentPkg();
	    
	    // Not the install version 
	    if (Cache[Pkg].InstallVer != *I || 
		(Cache[Pkg].Keep() == true && Pkg.State() == PkgIterator::NeedsNothing))
	       continue;
	       
	    if (List->IsFlag(Pkg,pkgOrderList::Configured)) {
	       Bad = false;
	       continue;
	    }

	    if (Debug)
	       clog << "Trying to SmartConfigure " << Pkg.Name() << endl;
	    Bad = !SmartConfigure(Pkg);
	 }

	 /* If this or element did not match then continue on to the
	    next or element until a matching element is found */
	 if (Bad == true)
	 {
	    // This triggers if someone make a pre-depends/depend loop.
	    if (Start == End)
	       return _error->Error("Couldn't configure pre-depend %s for %s, "
				    "probably a dependency cycle.",
				    End.TargetPkg().Name(),Pkg.Name());
	    Start++;
	 }
	 else 
	    break;
      }
      
      if (End->Type == pkgCache::Dep::Conflicts || 
	  End->Type == pkgCache::Dep::Obsoletes)
      {
	 /* Look for conflicts. Two packages that are both in the install
	    state cannot conflict so we don't check.. */
	 SPtrArray<Version *> VList = End.AllTargets();
	 for (Version **I = VList; *I != 0; I++)
	 {
	    VerIterator Ver(Cache,*I);
	    PkgIterator ConflictPkg = Ver.ParentPkg();
	    VerIterator InstallVer(Cache,Cache[ConflictPkg].InstallVer);
	    
	    // See if the current version is conflicting
	    if (ConflictPkg.CurrentVer() == Ver && List->IsNow(ConflictPkg))
	    {
	    	if (Debug && false) 
	         cout << " " << Pkg.Name() << " conflicts with " << ConflictPkg.Name() << endl;
	         
	   if (Debug && false) {
	       if (Ver==0) {
	          cout << "  Checking if " << Ver << " of " << ConflictPkg.Name() << " satisfies this dependancy" << endl;
	       } else {
	          cout << "  Checking if " << Ver.VerStr() << " of " << ConflictPkg.Name() << " satisfies this dependancy" << endl;
               }
            
               if (ConflictPkg.CurrentVer()==0) {
                  cout << "    CurrentVer " << ConflictPkg.CurrentVer() << " IsNow " << List->IsNow(ConflictPkg) << " NeedsNothing " << (ConflictPkg.State() == PkgIterator::NeedsNothing) << endl;
               } else { 
                  cout << "    CurrentVer " << ConflictPkg.CurrentVer().VerStr() << " IsNow " << List->IsNow(ConflictPkg) << " NeedsNothing " << (ConflictPkg.State() == PkgIterator::NeedsNothing) << endl;
               }
            
               if (InstallVer==0) {
                  cout << "    InstallVer " << InstallVer << endl;
               } else { 
                  cout << "    InstallVer " << InstallVer.VerStr() << endl; 
               }

               cout << "    Keep " << Cache[ConflictPkg].Keep() << " Unpacked " << List->IsFlag(ConflictPkg,pkgOrderList::UnPacked) << " Configured " << List->IsFlag(ConflictPkg,pkgOrderList::Configured) << " Removed " << List->IsFlag(ConflictPkg,pkgOrderList::Removed) << " Loop " << List->IsFlag(ConflictPkg,pkgOrderList::Loop) << endl;
               cout << "    Delete " << Cache[ConflictPkg].Delete() << endl;
            }
	    
	       if (!List->IsFlag(ConflictPkg,pkgOrderList::Loop)) {
	          if (Cache[ConflictPkg].Keep() == 0 && Cache[ConflictPkg].InstallVer != 0) {
	              if (Debug)
                        cout << "Unpacking " << ConflictPkg.Name() << " to prevent conflict" << endl;
	              List->Flag(Pkg,pkgOrderList::Loop);
	              SmartUnPack(ConflictPkg,false);
                  } else {
                      if (EarlyRemove(ConflictPkg) == false)
                         return _error->Error("Internal Error, Could not early remove %s",ConflictPkg.Name());
                  }
	       } else {
	          if (!List->IsFlag(ConflictPkg,pkgOrderList::Removed)) {
	              if (Debug)
                         cout << "Because of conficts knot, removing " << ConflictPkg.Name() << " to conflict violation" << endl;
	              if (EarlyRemove(ConflictPkg) == false)
                          return _error->Error("Internal Error, Could not early remove %s",ConflictPkg.Name());
	          }
	       }
	    }
	 }
      }
      
      // Check for breaks
      if (End->Type == pkgCache::Dep::DpkgBreaks) {
         SPtrArray<Version *> VList = End.AllTargets();
	 for (Version **I = VList; *I != 0; I++)
	 {
	    VerIterator Ver(Cache,*I);
	    PkgIterator BrokenPkg = Ver.ParentPkg();
	    VerIterator InstallVer(Cache,Cache[BrokenPkg].InstallVer);
	    
	    if (Debug && false) {
	       if (Ver==0) {
	          cout << "  Checking if " << Ver << " of " << BrokenPkg.Name() << " satisfies this dependancy" << endl;
	       } else {
	          cout << "  Checking if " << Ver.VerStr() << " of " << BrokenPkg.Name() << " satisfies this dependancy" << endl;
               }
            
               if (BrokenPkg.CurrentVer()==0) {
                  cout << "    CurrentVer " << BrokenPkg.CurrentVer() << " IsNow " << List->IsNow(BrokenPkg) << " NeedsNothing " << (BrokenPkg.State() == PkgIterator::NeedsNothing) << endl;
               } else { 
                  cout << "    CurrentVer " << BrokenPkg.CurrentVer().VerStr() << " IsNow " << List->IsNow(BrokenPkg) << " NeedsNothing " << (BrokenPkg.State() == PkgIterator::NeedsNothing) << endl;
               }
            
               if (InstallVer==0) {
                  cout << "    InstallVer " << InstallVer << endl;
               } else { 
                  cout << "    InstallVer " << InstallVer.VerStr() << endl; 
               }

               cout << "    Keep " << Cache[BrokenPkg].Keep() << " Unpacked " << List->IsFlag(BrokenPkg,pkgOrderList::UnPacked) << " Configured " << List->IsFlag(BrokenPkg,pkgOrderList::Configured) << " Removed " << List->IsFlag(BrokenPkg,pkgOrderList::Removed) << " Loop " << List->IsFlag(BrokenPkg,pkgOrderList::Loop) << " InList " << List->IsFlag(BrokenPkg,pkgOrderList::InList) << endl;
               cout << "    Delete " << Cache[BrokenPkg].Delete() << endl;
            }
	    // Check if it needs to be unpacked
	    if (List->IsFlag(BrokenPkg,pkgOrderList::InList) && Cache[BrokenPkg].Delete() == false && 
	        !List->IsFlag(BrokenPkg,pkgOrderList::Loop) && List->IsNow(BrokenPkg)) {
              List->Flag(Pkg,pkgOrderList::Loop);
	      // Found a break, so unpack the package
	      if (Debug) 
	         cout << "  Unpacking " << BrokenPkg.Name() << " to avoid break" << endl;
	      /*  */
	      SmartUnPack(BrokenPkg, false);
	    }
	    // Check if a package needs to be removed
	    if (Cache[BrokenPkg].Delete() == true && !List->IsFlag(BrokenPkg,pkgOrderList::Configured)) {
	      if (Debug) 
	         cout << "  Removing " << BrokenPkg.Name() << " to avoid break" << endl;
	      SmartRemove(BrokenPkg);
	    }
	 }
      }
   }
   
   // FIXME: Crude but effective fix, allows the SmartUnPack method to be used for packages that new to the system
   if (instVer != 0) {
     //cout << "Check for reverse conflicts on " << Pkg.Name() << " " << instVer.VerStr() << endl;
   
     // Check for reverse conflicts.
     if (CheckRConflicts(Pkg,Pkg.RevDependsList(),
		   instVer.VerStr()) == false)
        return false;
   
     for (PrvIterator P = instVer.ProvidesList();
	  P.end() == false; P++)
        CheckRConflicts(Pkg,P.ParentPkg().RevDependsList(),P.ProvideVersion());

     List->Flag(Pkg,pkgOrderList::UnPacked,pkgOrderList::States);

     if (instVer->MultiArch == pkgCache::Version::Same)
        for (PkgIterator P = Pkg.Group().PackageList();
  	     P.end() == false; P = Pkg.Group().NextPkg(P))
        {
	   if (Pkg == P || List->IsFlag(P,pkgOrderList::UnPacked) == true ||
	       Cache[P].InstallVer == 0 || (P.CurrentVer() == Cache[P].InstallVer &&
	        (Cache[Pkg].iFlags & pkgDepCache::ReInstall) != pkgDepCache::ReInstall))
	      continue;
	   SmartUnPack(P, false);
        }
      
   } else {
       VerIterator InstallVer(Cache,Cache[Pkg].InstallVer);
       //cout << "Check for reverse conflicts on " << Pkg.Name() << " " << InstallVer.VerStr() << endl;
   
       // Check for reverse conflicts.
       if (CheckRConflicts(Pkg,Pkg.RevDependsList(),
		   InstallVer.VerStr()) == false)
        return false;
        
      List->Flag(Pkg,pkgOrderList::UnPacked,pkgOrderList::States);
   }

   if(Install(Pkg,FileNames[Pkg->ID]) == false)
      return false;

   if (Immediate == true && List->IsFlag(Pkg,pkgOrderList::Immediate) == true) {
   
      // Perform immedate configuration of the package. 
         if (SmartConfigure(Pkg) == false)
            _error->Warning(_("Could not perform immediate configuration on '%s'. "
               "Please see man 5 apt.conf under APT::Immediate-Configure for details. (%d)"),Pkg.Name(),2);
   }
   
   return true;
}
									/*}}}*/
// PM::OrderInstall - Installation ordering routine			/*{{{*/
// ---------------------------------------------------------------------
/* */
pkgPackageManager::OrderResult pkgPackageManager::OrderInstall()
{
   if (CreateOrderList() == false)
      return Failed;

   Reset();
   
   if (Debug == true)
      clog << "Beginning to order" << endl;

   bool const ordering =
	_config->FindB("PackageManager::UnpackAll",true) ?
		List->OrderUnpack(FileNames) : List->OrderCritical();
   if (ordering == false)
   {
      _error->Error("Internal ordering error");
      return Failed;
   }
   
   if (Debug == true)
      clog << "Done ordering" << endl;

   bool DoneSomething = false;
   for (pkgOrderList::iterator I = List->begin(); I != List->end(); I++)
   {
      PkgIterator Pkg(Cache,*I);
      
      if (List->IsNow(Pkg) == false)
      {
         if (!List->IsFlag(Pkg,pkgOrderList::Configured) && !NoImmConfigure) {
            if (SmartConfigure(Pkg) == false && Debug)
               _error->Warning("Internal Error, Could not configure %s",Pkg.Name());
            // FIXME: The above warning message might need changing
         } else {
	    if (Debug == true)
	       clog << "Skipping already done " << Pkg.Name() << endl;
	 }
	 continue;
	 
      }
      
      if (List->IsMissing(Pkg) == true)
      {
	 if (Debug == true)
	    clog << "Sequence completed at " << Pkg.Name() << endl;
	 if (DoneSomething == false)
	 {
	    _error->Error("Internal Error, ordering was unable to handle the media swap");
	    return Failed;
	 }	 
	 return Incomplete;
      }
      
      // Sanity check
      if (Cache[Pkg].Keep() == true && 
	  Pkg.State() == pkgCache::PkgIterator::NeedsNothing &&
	  (Cache[Pkg].iFlags & pkgDepCache::ReInstall) != pkgDepCache::ReInstall)
      {
	 _error->Error("Internal Error, trying to manipulate a kept package (%s)",Pkg.Name());
	 return Failed;
      }
      
      // Perform a delete or an install
      if (Cache[Pkg].Delete() == true)
      {
	 if (SmartRemove(Pkg) == false)
	    return Failed;
      }
      else
	 if (SmartUnPack(Pkg) == false)
	    return Failed;
      DoneSomething = true;
      
      if (ImmConfigureAll) {
         /* ConfigureAll here to pick up and packages left unconfigured becuase they were unpacked in the 
            "PreUnpack Checks" section */
         ConfigureAll();
      }
   }

   // Final run through the configure phase
   if (ConfigureAll() == false)
      return Failed;

   // Sanity check
   for (pkgOrderList::iterator I = List->begin(); I != List->end(); I++)
   {
      if (List->IsFlag(*I,pkgOrderList::Configured) == false)
      {
	 _error->Error("Internal error, packages left unconfigured. %s",
		       PkgIterator(Cache,*I).Name());
	 return Failed;
      }
   }
	 
   return Completed;
}
									/*}}}*/
// PM::DoInstallPostFork - Does install part that happens after the fork /*{{{*/
// ---------------------------------------------------------------------
pkgPackageManager::OrderResult 
pkgPackageManager::DoInstallPostFork(int statusFd)
{
      if(statusFd > 0)
         // FIXME: use SetCloseExec here once it taught about throwing
	 //        exceptions instead of doing _exit(100) on failure
	 fcntl(statusFd,F_SETFD,FD_CLOEXEC); 
      bool goResult = Go(statusFd);
      if(goResult == false) 
	 return Failed;

      return Res;
};

// PM::DoInstall - Does the installation				/*{{{*/
// ---------------------------------------------------------------------
/* This uses the filenames in FileNames and the information in the
   DepCache to perform the installation of packages.*/
pkgPackageManager::OrderResult pkgPackageManager::DoInstall(int statusFd)
{
   if(DoInstallPreFork() == Failed)
      return Failed;
   
   return DoInstallPostFork(statusFd);
}
									/*}}}*/	      
